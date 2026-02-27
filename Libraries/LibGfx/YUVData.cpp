/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/YUVData.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>
#include <libyuv.h>

namespace Gfx {

namespace Details {

struct YUVDataImpl {
    IntSize size;
    u8 bit_depth;
    Media::Subsampling subsampling;
    Media::CodingIndependentCodePoints cicp;

    FixedArray<u8> y_buffer;
    FixedArray<u8> u_buffer;
    FixedArray<u8> v_buffer;

    // Lazily created when ImmutableBitmap needs it
    mutable Optional<SkYUVAPixmaps> pixmaps;

    SkYUVColorSpace skia_yuv_color_space() const
    {
        bool full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;

        switch (cicp.matrix_coefficients()) {
        case Media::MatrixCoefficients::BT470BG:
        case Media::MatrixCoefficients::BT601:
            return full_range ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
        case Media::MatrixCoefficients::BT709:
            return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
        case Media::MatrixCoefficients::BT2020NonConstantLuminance:
        case Media::MatrixCoefficients::BT2020ConstantLuminance:
            if (bit_depth <= 8)
                return kBT2020_8bit_Limited_SkYUVColorSpace;
            else if (bit_depth <= 10)
                return kBT2020_10bit_Limited_SkYUVColorSpace;
            else
                return kBT2020_12bit_Limited_SkYUVColorSpace;
        case Media::MatrixCoefficients::Identity:
            return kIdentity_SkYUVColorSpace;
        default:
            // Default to BT.709 for unsupported matrix coefficients
            return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
        }
    }

    SkYUVAInfo::Subsampling skia_subsampling() const
    {
        // Map Media::Subsampling to Skia's subsampling enum
        // x() = horizontal subsampling, y() = vertical subsampling
        if (!subsampling.x() && !subsampling.y())
            return SkYUVAInfo::Subsampling::k444; // 4:4:4 - no subsampling
        if (subsampling.x() && !subsampling.y())
            return SkYUVAInfo::Subsampling::k422; // 4:2:2 - horizontal only
        if (!subsampling.x() && subsampling.y())
            return SkYUVAInfo::Subsampling::k440; // 4:4:0 - vertical only
        return SkYUVAInfo::Subsampling::k420;     // 4:2:0 - both
    }

    SkYUVAPixmaps const& get_or_create_pixmaps() const
    {
        if (pixmaps.has_value())
            return pixmaps.value();

        auto skia_size = SkISize::Make(size.width(), size.height());

        // Use Y_U_V plane configuration (3 separate planes)
        auto yuva_info = SkYUVAInfo(
            skia_size,
            SkYUVAInfo::PlaneConfig::kY_U_V,
            skia_subsampling(),
            skia_yuv_color_space());

        // Determine color type based on bit depth
        SkColorType color_type;
        if (bit_depth <= 8) {
            color_type = kAlpha_8_SkColorType;
        } else {
            // 10/12/16-bit data stored in 16-bit values
            color_type = kA16_unorm_SkColorType;
        }

        // Calculate row bytes for each plane
        auto component_size = bit_depth <= 8 ? 1 : 2;
        auto y_row_bytes = static_cast<size_t>(size.width()) * component_size;

        auto uv_size = subsampling.subsampled_size(size);
        auto uv_row_bytes = static_cast<size_t>(uv_size.width()) * component_size;

        // Create pixmap info for each plane
        SkYUVAPixmapInfo::DataType data_type = bit_depth <= 8
            ? SkYUVAPixmapInfo::DataType::kUnorm8
            : SkYUVAPixmapInfo::DataType::kUnorm16;

        SkYUVAPixmapInfo pixmap_info(yuva_info, data_type, nullptr);

        // Create pixmaps from our buffers
        SkPixmap y_pixmap(
            SkImageInfo::Make(size.width(), size.height(), color_type, kOpaque_SkAlphaType),
            y_buffer.data(),
            y_row_bytes);
        SkPixmap u_pixmap(
            SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
            u_buffer.data(),
            uv_row_bytes);
        SkPixmap v_pixmap(
            SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
            v_buffer.data(),
            uv_row_bytes);

        SkPixmap plane_pixmaps[SkYUVAInfo::kMaxPlanes] = { y_pixmap, u_pixmap, v_pixmap, {} };

        pixmaps = SkYUVAPixmaps::FromExternalPixmaps(yuva_info, plane_pixmaps);
        return pixmaps.value();
    }
};

}

ErrorOr<NonnullOwnPtr<YUVData>> YUVData::create(IntSize size, u8 bit_depth, Media::Subsampling subsampling, Media::CodingIndependentCodePoints cicp)
{
    VERIFY(bit_depth <= 16);
    auto component_size = bit_depth <= 8 ? 1 : 2;

    auto y_buffer_size = static_cast<size_t>(size.width()) * size.height() * component_size;

    auto uv_size = subsampling.subsampled_size(size);
    auto uv_buffer_size = static_cast<size_t>(uv_size.width()) * uv_size.height() * component_size;

    auto y_buffer = TRY(FixedArray<u8>::create(y_buffer_size));
    auto u_buffer = TRY(FixedArray<u8>::create(uv_buffer_size));
    auto v_buffer = TRY(FixedArray<u8>::create(uv_buffer_size));

    auto impl = TRY(try_make<Details::YUVDataImpl>(Details::YUVDataImpl {
        .size = size,
        .bit_depth = bit_depth,
        .subsampling = subsampling,
        .cicp = cicp,
        .y_buffer = move(y_buffer),
        .u_buffer = move(u_buffer),
        .v_buffer = move(v_buffer),
        .pixmaps = {},
    }));

    return adopt_nonnull_own_or_enomem(new (nothrow) YUVData(move(impl)));
}

YUVData::YUVData(NonnullOwnPtr<Details::YUVDataImpl> impl)
    : m_impl(move(impl))
{
}

YUVData::~YUVData() = default;

IntSize YUVData::size() const
{
    return m_impl->size;
}

u8 YUVData::bit_depth() const
{
    return m_impl->bit_depth;
}

Media::Subsampling YUVData::subsampling() const
{
    return m_impl->subsampling;
}

Media::CodingIndependentCodePoints const& YUVData::cicp() const
{
    return m_impl->cicp;
}

Bytes YUVData::y_data()
{
    return m_impl->y_buffer.span();
}

Bytes YUVData::u_data()
{
    return m_impl->u_buffer.span();
}

Bytes YUVData::v_data()
{
    return m_impl->v_buffer.span();
}

static libyuv::YuvConstants const* yuv_constants_for_cicp(Media::CodingIndependentCodePoints const& cicp)
{
    bool full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;

    switch (cicp.matrix_coefficients()) {
    case Media::MatrixCoefficients::BT470BG:
    case Media::MatrixCoefficients::BT601:
        return full_range ? &libyuv::kYuvJPEGConstants : &libyuv::kYuvI601Constants;
    case Media::MatrixCoefficients::BT709:
    case Media::MatrixCoefficients::Unspecified:
        return full_range ? &libyuv::kYuvF709Constants : &libyuv::kYuvH709Constants;
    case Media::MatrixCoefficients::BT2020NonConstantLuminance:
    case Media::MatrixCoefficients::BT2020ConstantLuminance:
        return full_range ? &libyuv::kYuvV2020Constants : &libyuv::kYuv2020Constants;
    case Media::MatrixCoefficients::Identity:
        // Identity is handled separately (GBR copy)
        return nullptr;
    default:
        dbgln("YUV->RGB: Unsupported matrix coefficients {}, falling back to BT.709", cicp.matrix_coefficients());
        return full_range ? &libyuv::kYuvF709Constants : &libyuv::kYuvH709Constants;
    }
}

static StringView subsampling_name(Media::Subsampling subsampling)
{
    if (!subsampling.x() && !subsampling.y())
        return "4:4:4"sv;
    if (subsampling.x() && !subsampling.y())
        return "4:2:2"sv;
    if (!subsampling.x() && subsampling.y())
        return "4:4:0"sv;
    return "4:2:0"sv;
}

ErrorOr<NonnullRefPtr<Bitmap>> YUVData::to_bitmap() const
{
    auto start = MonotonicTime::now();

    auto const& impl = *m_impl;
    auto bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Premultiplied, impl.size));
    auto* dst = bitmap->scanline(0);
    auto dst_stride = static_cast<int>(bitmap->pitch());

    auto width = impl.size.width();
    auto height = impl.size.height();

    auto uv_size = impl.subsampling.subsampled_size(impl.size);

    if (impl.cicp.matrix_coefficients() == Media::MatrixCoefficients::Identity) {
        // Identity matrix: YUV planes are actually GBR. Copy Y→G, U→B, V→R.
        if (impl.bit_depth <= 8) {
            auto const* y_data = impl.y_buffer.data();
            auto const* u_data = impl.u_buffer.data();
            auto const* v_data = impl.v_buffer.data();
            auto y_stride = width;

            for (int row = 0; row < height; row++) {
                auto* dst_row = reinterpret_cast<u8*>(dst) + (static_cast<ptrdiff_t>(row) * dst_stride);
                auto const* y_row = y_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* u_row = u_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* v_row = v_data + (static_cast<ptrdiff_t>(row) * y_stride);
                for (int col = 0; col < width; col++) {
                    // BGRA8888 byte order: B, G, R, A
                    dst_row[(col * 4) + 0] = u_row[col]; // B
                    dst_row[(col * 4) + 1] = y_row[col]; // G
                    dst_row[(col * 4) + 2] = v_row[col]; // R
                    dst_row[(col * 4) + 3] = 255;        // A
                }
            }
        } else {
            auto shift = 16 - impl.bit_depth;
            auto const* y_data = reinterpret_cast<u16 const*>(impl.y_buffer.data());
            auto const* u_data = reinterpret_cast<u16 const*>(impl.u_buffer.data());
            auto const* v_data = reinterpret_cast<u16 const*>(impl.v_buffer.data());
            auto y_stride = width;

            for (int row = 0; row < height; row++) {
                auto* dst_row = reinterpret_cast<u8*>(dst) + (static_cast<ptrdiff_t>(row) * dst_stride);
                auto const* y_row = y_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* u_row = u_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* v_row = v_data + (static_cast<ptrdiff_t>(row) * y_stride);
                for (int col = 0; col < width; col++) {
                    // Convert from bit-replicated 16-bit to 8-bit
                    dst_row[(col * 4) + 0] = static_cast<u8>(u_row[col] >> (8 + shift)); // B
                    dst_row[(col * 4) + 1] = static_cast<u8>(y_row[col] >> (8 + shift)); // G
                    dst_row[(col * 4) + 2] = static_cast<u8>(v_row[col] >> (8 + shift)); // R
                    dst_row[(col * 4) + 3] = 255;                                        // A
                }
            }
        }

        auto elapsed = MonotonicTime::now() - start;
        dbgln("YUV->RGB CPU conversion (Identity): {}x{} {}bit {}: {}",
            width, height, impl.bit_depth, subsampling_name(impl.subsampling), elapsed);
        return bitmap;
    }

    auto const* yuv_constants = yuv_constants_for_cicp(impl.cicp);
    VERIFY(yuv_constants);

    if (impl.bit_depth <= 8) {
        auto const* y_data = impl.y_buffer.data();
        auto const* u_data = impl.u_buffer.data();
        auto const* v_data = impl.v_buffer.data();
        auto y_stride = width;
        auto uv_stride = uv_size.width();

        int result;
        if (impl.subsampling.x() && impl.subsampling.y()) {
            // 4:2:0
            result = libyuv::I420ToARGBMatrix(y_data, y_stride, u_data, uv_stride, v_data, uv_stride,
                reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
        } else if (impl.subsampling.x() && !impl.subsampling.y()) {
            // 4:2:2
            result = libyuv::I422ToARGBMatrix(y_data, y_stride, u_data, uv_stride, v_data, uv_stride,
                reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
        } else {
            // 4:4:4 (or 4:4:0 treated as 4:4:4)
            result = libyuv::I444ToARGBMatrix(y_data, y_stride, u_data, uv_stride, v_data, uv_stride,
                reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
        }

        if (result != 0)
            return Error::from_string_literal("libyuv YUV-to-RGB conversion failed");
    } else {
        // High bit depth (10/12-bit stored in 16-bit values, bit-replicated to fill 16 bits)
        // libyuv 10-bit functions expect values in 0-1023 range, 12-bit in 0-4095 range.
        // Our data is bit-replicated to 16-bit, so we need to right-shift.
        auto const* y_data_16 = reinterpret_cast<u16 const*>(impl.y_buffer.data());
        auto const* u_data_16 = reinterpret_cast<u16 const*>(impl.u_buffer.data());
        auto const* v_data_16 = reinterpret_cast<u16 const*>(impl.v_buffer.data());
        auto y_stride_16 = width;
        auto uv_stride_16 = uv_size.width();

        int shift;
        if (impl.bit_depth <= 10)
            shift = 6; // 16-bit to 10-bit
        else
            shift = 4; // 16-bit to 12-bit

        // Create temporary shifted buffers
        auto y_pixel_count = static_cast<size_t>(width) * height;
        auto uv_pixel_count = static_cast<size_t>(uv_size.width()) * uv_size.height();

        auto shifted_y = TRY(FixedArray<u16>::create(y_pixel_count));
        auto shifted_u = TRY(FixedArray<u16>::create(uv_pixel_count));
        auto shifted_v = TRY(FixedArray<u16>::create(uv_pixel_count));

        for (size_t i = 0; i < y_pixel_count; i++)
            shifted_y[i] = y_data_16[i] >> shift;
        for (size_t i = 0; i < uv_pixel_count; i++) {
            shifted_u[i] = u_data_16[i] >> shift;
            shifted_v[i] = v_data_16[i] >> shift;
        }

        // Strides are in bytes for libyuv 10/12-bit functions (u16 elements, so multiply by 2)
        auto y_stride_bytes = y_stride_16 * 2;
        auto uv_stride_bytes = uv_stride_16 * 2;

        int result;
        if (impl.bit_depth <= 10) {
            if (impl.subsampling.x() && impl.subsampling.y()) {
                result = libyuv::I010ToARGBMatrix(shifted_y.data(), y_stride_bytes, shifted_u.data(), uv_stride_bytes,
                    shifted_v.data(), uv_stride_bytes, reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
            } else if (impl.subsampling.x() && !impl.subsampling.y()) {
                result = libyuv::I210ToARGBMatrix(shifted_y.data(), y_stride_bytes, shifted_u.data(), uv_stride_bytes,
                    shifted_v.data(), uv_stride_bytes, reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
            } else {
                result = libyuv::I410ToARGBMatrix(shifted_y.data(), y_stride_bytes, shifted_u.data(), uv_stride_bytes,
                    shifted_v.data(), uv_stride_bytes, reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
            }
        } else {
            // 12-bit
            if (impl.subsampling.x() && impl.subsampling.y()) {
                result = libyuv::I012ToARGBMatrix(shifted_y.data(), y_stride_bytes, shifted_u.data(), uv_stride_bytes,
                    shifted_v.data(), uv_stride_bytes, reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
            } else if (impl.subsampling.x() && !impl.subsampling.y()) {
                // No I212 in libyuv, use I210 with values already shifted to 10-bit
                // Re-shift from 12-bit to 10-bit (>> 2 more)
                for (size_t i = 0; i < y_pixel_count; i++)
                    shifted_y[i] >>= 2;
                for (size_t i = 0; i < uv_pixel_count; i++) {
                    shifted_u[i] >>= 2;
                    shifted_v[i] >>= 2;
                }
                result = libyuv::I210ToARGBMatrix(shifted_y.data(), y_stride_bytes, shifted_u.data(), uv_stride_bytes,
                    shifted_v.data(), uv_stride_bytes, reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
            } else {
                // No I412 in libyuv, use I410 with values shifted to 10-bit
                for (size_t i = 0; i < y_pixel_count; i++)
                    shifted_y[i] >>= 2;
                for (size_t i = 0; i < uv_pixel_count; i++) {
                    shifted_u[i] >>= 2;
                    shifted_v[i] >>= 2;
                }
                result = libyuv::I410ToARGBMatrix(shifted_y.data(), y_stride_bytes, shifted_u.data(), uv_stride_bytes,
                    shifted_v.data(), uv_stride_bytes, reinterpret_cast<u8*>(dst), dst_stride, yuv_constants, width, height);
            }
        }

        if (result != 0)
            return Error::from_string_literal("libyuv YUV-to-RGB conversion failed");
    }

    auto elapsed = MonotonicTime::now() - start;
    dbgln("YUV->RGB CPU conversion: {}x{} {}bit {}: {}",
        width, height, impl.bit_depth, subsampling_name(impl.subsampling), elapsed);
    return bitmap;
}

SkYUVAPixmaps const& YUVData::skia_yuva_pixmaps() const
{
    return m_impl->get_or_create_pixmaps();
}

}

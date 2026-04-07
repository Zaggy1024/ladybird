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
#include <RustFFI.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

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

static FFI::YUVMatrix yuv_matrix_for_cicp(Media::CodingIndependentCodePoints const& cicp)
{
    switch (cicp.matrix_coefficients()) {
    case Media::MatrixCoefficients::Identity:
        return FFI::YUVMatrix::Identity;
    case Media::MatrixCoefficients::FCC:
        return FFI::YUVMatrix::Fcc;
    case Media::MatrixCoefficients::BT470BG:
        return FFI::YUVMatrix::Bt470BG;
    case Media::MatrixCoefficients::BT601:
        return FFI::YUVMatrix::Bt601;
    case Media::MatrixCoefficients::SMPTE240:
        return FFI::YUVMatrix::Smpte240;
    case Media::MatrixCoefficients::BT2020NonConstantLuminance:
    case Media::MatrixCoefficients::BT2020ConstantLuminance:
        return FFI::YUVMatrix::Bt2020;
    case Media::MatrixCoefficients::BT709:
    case Media::MatrixCoefficients::Unspecified:
    default:
        return FFI::YUVMatrix::Bt709;
    }
}

ErrorOr<NonnullRefPtr<Bitmap>> YUVData::to_bitmap() const
{
    auto start = MonotonicTime::now();

    auto const& impl = *m_impl;
    auto bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Premultiplied, impl.size));
    auto* dst = reinterpret_cast<u8*>(bitmap->scanline(0));
    auto dst_stride = static_cast<u32>(bitmap->pitch());

    auto width = static_cast<u32>(impl.size.width());
    auto height = static_cast<u32>(impl.size.height());

    auto uv_size = impl.subsampling.subsampled_size(impl.size);

    bool full_range = impl.cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;
    auto range = full_range ? FFI::YUVRange::Full : FFI::YUVRange::Limited;
    auto matrix = yuv_matrix_for_cicp(impl.cicp);

    if (impl.cicp.matrix_coefficients() == Media::MatrixCoefficients::Identity) {
        // Identity matrix: YUV planes are actually GBR. Copy Y->G, U->B, V->R.
        // Output is RGBA byte order to match the non-identity path.
        if (impl.bit_depth <= 8) {
            auto const* y_data = impl.y_buffer.data();
            auto const* u_data = impl.u_buffer.data();
            auto const* v_data = impl.v_buffer.data();
            auto y_stride = static_cast<int>(width);

            for (u32 row = 0; row < height; row++) {
                auto* dst_row = dst + (static_cast<ptrdiff_t>(row) * dst_stride);
                auto const* y_row = y_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* u_row = u_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* v_row = v_data + (static_cast<ptrdiff_t>(row) * y_stride);
                for (u32 col = 0; col < width; col++) {
                    dst_row[(col * 4) + 0] = v_row[col]; // R
                    dst_row[(col * 4) + 1] = y_row[col]; // G
                    dst_row[(col * 4) + 2] = u_row[col]; // B
                    dst_row[(col * 4) + 3] = 255;        // A
                }
            }
        } else {
            auto shift = 16 - impl.bit_depth;
            auto const* y_data = reinterpret_cast<u16 const*>(impl.y_buffer.data());
            auto const* u_data = reinterpret_cast<u16 const*>(impl.u_buffer.data());
            auto const* v_data = reinterpret_cast<u16 const*>(impl.v_buffer.data());
            auto y_stride = static_cast<int>(width);

            for (u32 row = 0; row < height; row++) {
                auto* dst_row = dst + (static_cast<ptrdiff_t>(row) * dst_stride);
                auto const* y_row = y_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* u_row = u_data + (static_cast<ptrdiff_t>(row) * y_stride);
                auto const* v_row = v_data + (static_cast<ptrdiff_t>(row) * y_stride);
                for (u32 col = 0; col < width; col++) {
                    dst_row[(col * 4) + 0] = static_cast<u8>(v_row[col] >> (8 + shift)); // R
                    dst_row[(col * 4) + 1] = static_cast<u8>(y_row[col] >> (8 + shift)); // G
                    dst_row[(col * 4) + 2] = static_cast<u8>(u_row[col] >> (8 + shift)); // B
                    dst_row[(col * 4) + 3] = 255;                                        // A
                }
            }
        }

        auto elapsed = MonotonicTime::now() - start;
        dbgln("YUV->RGB CPU conversion (Identity): {}x{} {}bit {}: {}ms",
            width, height, impl.bit_depth, subsampling_name(impl.subsampling), elapsed.to_milliseconds());
        return bitmap;
    }

    bool success;
    if (impl.bit_depth <= 8) {
        auto y_stride = static_cast<u32>(width);
        auto uv_stride = static_cast<u32>(uv_size.width());

        success = FFI::yuv_to_bgra_8bit(
            impl.y_buffer.data(), y_stride,
            impl.u_buffer.data(), uv_stride,
            impl.v_buffer.data(), uv_stride,
            width, height,
            impl.subsampling.x(), impl.subsampling.y(),
            dst, dst_stride,
            range, matrix);
    } else {
        // Right-shift bit-replicated 16-bit values to native bit depth
        auto shift = 16 - impl.bit_depth;

        auto y_pixel_count = static_cast<size_t>(width) * height;
        auto uv_pixel_count = static_cast<size_t>(uv_size.width()) * uv_size.height();

        auto shifted_y = TRY(FixedArray<u16>::create(y_pixel_count));
        auto shifted_u = TRY(FixedArray<u16>::create(uv_pixel_count));
        auto shifted_v = TRY(FixedArray<u16>::create(uv_pixel_count));

        auto const* y_data_16 = reinterpret_cast<u16 const*>(impl.y_buffer.data());
        auto const* u_data_16 = reinterpret_cast<u16 const*>(impl.u_buffer.data());
        auto const* v_data_16 = reinterpret_cast<u16 const*>(impl.v_buffer.data());

        for (size_t i = 0; i < y_pixel_count; i++)
            shifted_y[i] = y_data_16[i] >> shift;
        for (size_t i = 0; i < uv_pixel_count; i++) {
            shifted_u[i] = u_data_16[i] >> shift;
            shifted_v[i] = v_data_16[i] >> shift;
        }

        // Strides in bytes for the u16 planes
        auto y_stride_bytes = static_cast<u32>(width * 2);
        auto uv_stride_bytes = static_cast<u32>(uv_size.width() * 2);

        success = FFI::yuv_to_bgra_high_bit_depth(
            shifted_y.data(), y_stride_bytes,
            shifted_u.data(), uv_stride_bytes,
            shifted_v.data(), uv_stride_bytes,
            width, height,
            impl.bit_depth,
            impl.subsampling.x(), impl.subsampling.y(),
            dst, dst_stride,
            range, matrix);
    }

    if (!success)
        return Error::from_string_literal("YUV-to-RGB conversion failed");

    auto elapsed = MonotonicTime::now() - start;
    dbgln("YUV->RGB CPU conversion: {}x{} {}bit {}: {}us",
        width, height, impl.bit_depth, subsampling_name(impl.subsampling), elapsed.to_microseconds());
    return bitmap;
}

SkYUVAPixmaps const& YUVData::skia_yuva_pixmaps() const
{
    return m_impl->get_or_create_pixmaps();
}

}

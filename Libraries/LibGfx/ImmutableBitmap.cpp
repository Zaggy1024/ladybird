/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>

#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkSurface.h>
#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/GpuTypes.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Gfx {

StringView export_format_name(ExportFormat format)
{
    switch (format) {
#define ENUMERATE_EXPORT_FORMAT(format) \
    case Gfx::ExportFormat::format:     \
        return #format##sv;
        ENUMERATE_EXPORT_FORMATS(ENUMERATE_EXPORT_FORMAT)
#undef ENUMERATE_EXPORT_FORMAT
    }
    VERIFY_NOT_REACHED();
}

struct ImmutableBitmapImpl {
    sk_sp<SkImage> sk_image;
    SkBitmap sk_bitmap;
    Variant<NonnullRefPtr<Gfx::Bitmap>, NonnullRefPtr<Gfx::PaintingSurface>, Empty> source;
    ColorSpace color_space;
};

int ImmutableBitmap::width() const
{
    return m_impl->sk_image->width();
}

int ImmutableBitmap::height() const
{
    return m_impl->sk_image->height();
}

IntRect ImmutableBitmap::rect() const
{
    return { {}, size() };
}

IntSize ImmutableBitmap::size() const
{
    return { width(), height() };
}

AlphaType ImmutableBitmap::alpha_type() const
{
    // We assume premultiplied alpha type for opaque surfaces since that is Skia's preferred alpha type and the
    // effective pixel data is identical between premultiplied and unpremultiplied in that case.
    return m_impl->sk_image->alphaType() == kUnpremul_SkAlphaType ? AlphaType::Unpremultiplied : AlphaType::Premultiplied;
}

SkImage const* ImmutableBitmap::sk_image() const
{
    return m_impl->sk_image.get();
}

static int bytes_per_pixel_for_export_format(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
    case ExportFormat::Alpha8:
        return 1;
    case ExportFormat::RGB565:
    case ExportFormat::RGBA5551:
    case ExportFormat::RGBA4444:
        return 2;
    case ExportFormat::RGB888:
        return 3;
    case ExportFormat::RGBA8888:
        return 4;
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkColorType export_format_to_skia_color_type(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
        return SkColorType::kGray_8_SkColorType;
    case ExportFormat::Alpha8:
        return SkColorType::kAlpha_8_SkColorType;
    case ExportFormat::RGB565:
        return SkColorType::kRGB_565_SkColorType;
    case ExportFormat::RGBA5551:
        dbgln("FIXME: Support conversion to RGBA5551.");
        return SkColorType::kUnknown_SkColorType;
    case ExportFormat::RGBA4444:
        return SkColorType::kARGB_4444_SkColorType;
    case ExportFormat::RGB888:
        // This one needs to be converted manually because Skia has no valid 24-bit color type.
        VERIFY_NOT_REACHED();
    case ExportFormat::RGBA8888:
        return SkColorType::kRGBA_8888_SkColorType;
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<BitmapExportResult> ImmutableBitmap::export_to_byte_buffer(ExportFormat format, int flags, Optional<int> target_width, Optional<int> target_height) const
{
    int width = target_width.value_or(this->width());
    int height = target_height.value_or(this->height());

    if (format == ExportFormat::RGB888 && (width != this->width() || height != this->height())) {
        dbgln("FIXME: Ignoring target width and height because scaling is not implemented for this export format.");
        width = this->width();
        height = this->height();
    }

    Checked<size_t> buffer_pitch = width;
    int number_of_bytes = bytes_per_pixel_for_export_format(format);
    buffer_pitch *= number_of_bytes;
    if (buffer_pitch.has_overflow())
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    if (width > 0 && height > 0) {
        if (format == ExportFormat::RGB888) {
            // 24 bit RGB is not supported by Skia, so we need to handle this format ourselves.
            auto raw_buffer = buffer.data();
            for (auto y = 0; y < height; y++) {
                auto target_y = flags & ExportFlags::FlipY ? height - y - 1 : y;
                for (auto x = 0; x < width; x++) {
                    auto pixel = get_pixel(x, y);
                    auto buffer_offset = (target_y * buffer_pitch.value()) + (x * 3ull);
                    raw_buffer[buffer_offset + 0] = pixel.red();
                    raw_buffer[buffer_offset + 1] = pixel.green();
                    raw_buffer[buffer_offset + 2] = pixel.blue();
                }
            }
        } else {
            auto skia_format = export_format_to_skia_color_type(format);
            auto color_space = SkColorSpace::MakeSRGB();

            auto image_info = SkImageInfo::Make(width, height, skia_format, flags & ExportFlags::PremultiplyAlpha ? SkAlphaType::kPremul_SkAlphaType : SkAlphaType::kUnpremul_SkAlphaType, color_space);
            auto surface = SkSurfaces::WrapPixels(image_info, buffer.data(), buffer_pitch.value());
            VERIFY(surface);
            auto* surface_canvas = surface->getCanvas();
            auto dst_rect = Gfx::to_skia_rect(Gfx::Rect { 0, 0, width, height });

            if (flags & ExportFlags::FlipY) {
                surface_canvas->translate(0, dst_rect.height());
                surface_canvas->scale(1, -1);
            }

            surface_canvas->drawImageRect(sk_image(), dst_rect, Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor));
        }
    } else {
        VERIFY(buffer.is_empty());
    }

    return BitmapExportResult {
        .buffer = move(buffer),
        .width = width,
        .height = height,
    };
}

RefPtr<Gfx::Bitmap const> ImmutableBitmap::bitmap() const
{
    // FIXME: Implement for PaintingSurface
    return m_impl->source.get<NonnullRefPtr<Gfx::Bitmap>>();
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    // FIXME: Implement for PaintingSurface
    return m_impl->source.get<NonnullRefPtr<Gfx::Bitmap>>()->get_pixel(x, y);
}

static SkAlphaType to_skia_alpha_type(Gfx::AlphaType alpha_type)
{
    switch (alpha_type) {
    case AlphaType::Premultiplied:
        return kPremul_SkAlphaType;
    case AlphaType::Unpremultiplied:
        return kUnpremul_SkAlphaType;
    default:
        VERIFY_NOT_REACHED();
    }
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space)
{
    ImmutableBitmapImpl impl;
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->alpha_type()), color_space.color_space<sk_sp<SkColorSpace>>());
    impl.sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(bitmap->scanline(0))), bitmap->pitch());
    impl.sk_bitmap.setImmutable();
    impl.sk_image = impl.sk_bitmap.asImage();
    impl.source = bitmap;
    impl.color_space = move(color_space);
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, AlphaType alpha_type, ColorSpace color_space)
{
    // Convert the source bitmap to the right alpha type on a mismatch. We want to do this when converting from a
    // Bitmap to an ImmutableBitmap, since at that point we usually know the right alpha type to use in context.
    auto source_bitmap = bitmap;
    if (source_bitmap->alpha_type() != alpha_type) {
        source_bitmap = MUST(bitmap->clone());
        source_bitmap->set_alpha_type_destructive(alpha_type);
    }

    return create(source_bitmap, move(color_space));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface> painting_surface)
{
    ImmutableBitmapImpl impl;
    impl.sk_image = painting_surface->sk_image_snapshot<sk_sp<SkImage>>();
    impl.source = painting_surface;
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

ErrorOr<NonnullRefPtr<ImmutableBitmap>> ImmutableBitmap::create_yuv(GrRecordingContext& context, int width, int height, u8 bit_depth, Media::CodingIndependentCodePoints cicp, Subsampling subsampling, ReadonlyBytes plane_y, ReadonlyBytes plane_u, ReadonlyBytes plane_v)
{
    auto sk_plane_config = SkYUVAInfo::PlaneConfig::kY_U_V;
    auto sk_subsampling = [&] {
        switch (subsampling) {
        case Subsampling::Y4CbCr44:
            return SkYUVAInfo::Subsampling::k444;
        case Subsampling::Y4CbCr40:
            return SkYUVAInfo::Subsampling::k440;
        case Subsampling::Y4CbCr22:
            return SkYUVAInfo::Subsampling::k422;
        case Subsampling::Y4CbCr20:
            return SkYUVAInfo::Subsampling::k420;
        case Subsampling::Y4CbCr11:
            return SkYUVAInfo::Subsampling::k411;
        }
        VERIFY_NOT_REACHED();
    }();
    auto sk_yuv_color_space = TRY([&] -> ErrorOr<SkYUVColorSpace> {
#define DISALLOW_HIGH_BIT_DEPTH(name)                                                                     \
    do {                                                                                                  \
        if (bit_depth != 8)                                                                               \
            return Error::from_string_literal(name " matrix coefficients do not support high bit depth"); \
    } while (false)

#define SELECT_RANGE(name) [&] {                                         \
    if (cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full) \
        return SkYUVColorSpace::k##name##_Full_SkYUVColorSpace;          \
    return SkYUVColorSpace::k##name##_Limited_SkYUVColorSpace;           \
}()
        switch (cicp.matrix_coefficients()) {
        case Media::MatrixCoefficients::BT470BG:
        case Media::MatrixCoefficients::BT601:
            DISALLOW_HIGH_BIT_DEPTH("BT.470 B/G and BT601");
            if (cicp.video_full_range_flag() != Media::VideoFullRangeFlag::Full)
                return Error::from_string_literal("BT.470 B/G and BT601 do not support full range");
            return SkYUVColorSpace::kRec601_Limited_SkYUVColorSpace;
        case Media::MatrixCoefficients::BT709:
            DISALLOW_HIGH_BIT_DEPTH("BT.709");
            VERIFY(bit_depth == 8);
            return SELECT_RANGE(Rec709);
        case Media::MatrixCoefficients::BT2020NonConstantLuminance:
        case Media::MatrixCoefficients::BT2020ConstantLuminance:
            switch (bit_depth) {
            case 8:
                return SELECT_RANGE(BT2020_8bit);
            case 10:
                return SELECT_RANGE(BT2020_10bit);
            case 12:
                return SELECT_RANGE(BT2020_12bit);
            default:
                return Error::from_string_literal("Unsupported bit BT.2020 bit depth");
            }
        default:
            return Error::from_string_literal("Unsupported matrix coefficients");
        }
    }());
    auto sk_transfer_function = TRY([&] -> ErrorOr<skcms_TransferFunction> {
        // FIXME: Use the named transfer functions from SkColorSpace.h when it is updated to include them.
        switch (cicp.transfer_characteristics()) {
        case Media::TransferCharacteristics::BT709:
        case Media::TransferCharacteristics::BT601:
        case Media::TransferCharacteristics::IEC61966:
        case Media::TransferCharacteristics::BT2020BitDepth10:
        case Media::TransferCharacteristics::BT2020BitDepth12:
            return skcms_TransferFunction(2.4f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::BT470M:
            return skcms_TransferFunction(2.2f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::BT470BG:
            return skcms_TransferFunction(2.8f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::SMPTE240:
            return skcms_TransferFunction(2.222222222222f, 0.899626676224f, 0.100373323776f, 0.25f, 0.091286342118f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::Linear:
            return skcms_TransferFunction(1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::SRGB:
            return skcms_TransferFunction(2.4f, 1.0f / 1.055f, 0.055f / 1.055f, 1.0f / 12.92f, 0.04045f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::SMPTE2084:
            return skcms_TransferFunction(-5.0f, 203.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::SMPTE428:
            return skcms_TransferFunction(2.6f, 1.034080527699f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        case Media::TransferCharacteristics::HLG:
            return skcms_TransferFunction(-6.0f, 203.f, 1000.0f, 1.2f, 0.0f, 0.0f, 0.0f);
        default:
            return Error::from_string_literal("Unsupported transfer characteristics");
        }
    }());
    auto sk_matrix = TRY([&] -> ErrorOr<skcms_Matrix3x3> {
        // FIXME: Use the named transfer functions from SkColorSpace.h when it is updated to include them.
        switch (cicp.transfer_characteristics()) {
        default:
            return Error::from_string_literal("Unsupported transfer characteristics");
        }
    }());
    auto sk_color_space = SkColorSpace::MakeRGB(sk_transfer_function, sk_matrix);

    auto info = SkYUVAInfo(
        SkISize::Make(width, height),
        sk_plane_config,
        sk_subsampling,
        sk_yuv_color_space);
    auto pixmap_info = SkYUVAPixmapInfo(info, SkYUVAPixmapInfo::DataType::kUnorm8, nullptr);
    auto pixmaps = SkYUVAPixmaps::Allocate(pixmap_info);

    auto const& y = pixmaps.plane(0);
    VERIFY(y.computeByteSize() >= plane_y.size());
    memcpy(y.writable_addr(), plane_y.data(), plane_y.size());

    auto const& u = pixmaps.plane(0);
    VERIFY(u.computeByteSize() >= plane_u.size());
    memcpy(u.writable_addr(), plane_u.data(), plane_u.size());

    auto const& v = pixmaps.plane(0);
    VERIFY(v.computeByteSize() >= plane_v.size());
    memcpy(v.writable_addr(), plane_v.data(), plane_v.size());

    ImmutableBitmapImpl impl;
    impl.sk_image = SkImages::TextureFromYUVAPixmaps(&context, pixmaps, skgpu::Mipmapped::kNo, false, sk_color_space);
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(impl)));
}

ImmutableBitmap::ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> impl)
    : m_impl(move(impl))
{
}

ImmutableBitmap::~ImmutableBitmap() = default;

}

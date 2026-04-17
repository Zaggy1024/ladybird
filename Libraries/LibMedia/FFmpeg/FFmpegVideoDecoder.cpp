/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>

#include "FFmpegHelpers.h"
#include "FFmpegVideoDecoder.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace Media::FFmpeg {

static bool is_planar_yuv_format(AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV420P12:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_YUV422P12:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_YUV444P12:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
        return true;
    default:
        return false;
    }
}

static bool is_semi_planar_yuv_format(AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_P010:
        return true;
    default:
        return false;
    }
}

static AVPixelFormat negotiate_output_format(AVCodecContext* codec_context, AVPixelFormat const* formats)
{
    // If hw decoding is configured, prefer the hw pixel format.
    if (codec_context->hw_device_ctx) {
        auto hw_type = reinterpret_cast<AVHWDeviceContext*>(codec_context->hw_device_ctx->data)->type;
        for (auto const* fmt = formats; *fmt >= 0; fmt++) {
            for (int i = 0; auto const* hw_config = avcodec_get_hw_config(codec_context->codec, i); i++) {
                if (hw_config->device_type == hw_type && hw_config->pix_fmt == *fmt)
                    return *fmt;
            }
        }
    }

    // Fall back to supported software formats.
    for (auto const* fmt = formats; *fmt >= 0; fmt++) {
        if (is_planar_yuv_format(*fmt))
            return *fmt;
    }
    return AV_PIX_FMT_NONE;
}

static constexpr AVHWDeviceType s_preferred_hw_device_types[] = {
#ifdef AK_OS_MACOS
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(AK_OS_LINUX)
    AV_HWDEVICE_TYPE_VAAPI,
#elif defined(AK_OS_WINDOWS)
    AV_HWDEVICE_TYPE_D3D11VA,
#endif
};

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVBufferRef* hw_device_ctx = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            avcodec_free_context(&codec_context);
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_buffer_unref(&hw_device_ctx);
        }
    };

    auto ff_codec_id = ffmpeg_codec_id_from_media_codec_id(codec_id);
    auto const* codec = avcodec_find_decoder(ff_codec_id);
    if (!codec)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find FFmpeg decoder for codec {}", codec_id);

    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    codec_context->get_format = negotiate_output_format;
    codec_context->time_base = { 1, 1'000'000 };
    codec_context->thread_count = static_cast<int>(min(Core::System::hardware_concurrency(), 4));

    // Try to set up hardware-accelerated decoding. Failure is non-fatal; we fall back to software.
    for (auto hw_type : s_preferred_hw_device_types) {
        if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, nullptr, nullptr, 0) == 0) {
            codec_context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            codec_context->hwaccel_flags |= AV_HWACCEL_FLAG_UNSAFE_OUTPUT;
            dbgln("FFmpegVideoDecoder: Using hardware decoder type {}", av_hwdevice_get_type_name(hw_type));
            break;
        }
    }

    if (!codec_initialization_data.is_empty()) {
        if (codec_initialization_data.size() > NumericLimits<int>::max())
            return DecoderError::corrupted("Codec initialization data is too large"sv);

        codec_context->extradata = static_cast<u8*>(av_malloc(codec_initialization_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codec_context->extradata)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate codec initialization data buffer for FFmpeg codec"sv);

        memcpy(codec_context->extradata, codec_initialization_data.data(), codec_initialization_data.size());
        codec_context->extradata_size = static_cast<int>(codec_initialization_data.size());
    }

    if (avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", codec_id);

    packet = av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    memory_guard.disarm();
    return DECODER_TRY_ALLOC(try_make<FFmpegVideoDecoder>(codec_context, packet, frame, hw_device_ctx));
}

FFmpegVideoDecoder::FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, AVBufferRef* hw_device_ctx)
    : m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
    , m_hw_device_ctx(hw_device_ctx)
{
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    av_packet_free(&m_packet);
    av_frame_free(&m_frame);
    avcodec_free_context(&m_codec_context);
    av_buffer_unref(&m_hw_device_ctx);
}

DecoderErrorOr<void> FFmpegVideoDecoder::send_packet(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data)
{
    VERIFY(coded_data.size() < NumericLimits<int>::max());

    m_packet->data = const_cast<u8*>(coded_data.data());
    m_packet->size = static_cast<int>(coded_data.size());
    m_packet->pts = timestamp.to_microseconds();
    m_packet->dts = m_packet->pts;
    m_packet->duration = duration.to_microseconds();

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    switch (result) {
    case 0:
        return {};
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder cannot decode any more data until frames have been retrieved"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
    case AVERROR(ENOMEM):
        return DecoderError::with_description(DecoderErrorCategory::Memory, "FFmpeg codec ran out of internal memory"sv);
    default:
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "FFmpeg codec reports that the data is corrupted"sv);
    }
}

DecoderErrorOr<void> FFmpegVideoDecoder::decode_for_reference(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data)
{
    TRY(send_packet(timestamp, duration, coded_data));

    // FFmpeg doesn't support "decode without output" — we still need to drain the produced
    // frames to advance internal state. Receive and discard.
    while (true) {
        auto result = avcodec_receive_frame(m_codec_context, m_frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            break;
        if (result < 0)
            return DecoderError::format(DecoderErrorCategory::Unknown, "avcodec_receive_frame failed: {}", result);
        av_frame_unref(m_frame);
    }
    return {};
}

DecoderErrorOr<void> FFmpegVideoDecoder::decode_for_output(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data, CodingIndependentCodePoints const& container_cicp)
{
    m_output_cicp = container_cicp;
    return send_packet(timestamp, duration, coded_data);
}

void FFmpegVideoDecoder::signal_end_of_stream()
{
    m_packet->data = nullptr;
    m_packet->size = 0;
    m_packet->pts = 0;
    m_packet->dts = 0;

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    VERIFY(result == 0 || result == AVERROR_EOF);
}

static CodingIndependentCodePoints extract_cicp(AVFrame* frame, CodingIndependentCodePoints const& container_cicp)
{
    auto color_primaries = static_cast<ColorPrimaries>(frame->color_primaries);
    auto transfer_characteristics = static_cast<TransferCharacteristics>(frame->color_trc);
    auto matrix_coefficients = static_cast<MatrixCoefficients>(frame->colorspace);
    auto color_range = [&] {
        switch (frame->color_range) {
        case AVColorRange::AVCOL_RANGE_MPEG:
            return VideoFullRangeFlag::Studio;
        case AVColorRange::AVCOL_RANGE_JPEG:
            return VideoFullRangeFlag::Full;
        default:
            return VideoFullRangeFlag::Unspecified;
        }
    }();
    auto cicp = CodingIndependentCodePoints { color_primaries, transfer_characteristics, matrix_coefficients, color_range };
    cicp.adopt_specified_values(container_cicp);
    return cicp;
}

DecoderErrorOr<NonnullOwnPtr<VideoFrame>> FFmpegVideoDecoder::materialize_frame(AVFrame* frame, CodingIndependentCodePoints const& cicp)
{
    auto size = Gfx::Size<u32> { frame->width, frame->height };
    auto timestamp = AK::Duration::from_microseconds(frame->pts);
    auto duration = AK::Duration::from_microseconds(frame->duration);

#ifdef AK_OS_MACOS
    if (frame->hw_frames_ctx && frame->data[3]) {
        auto color_space_or_error = Gfx::ColorSpace::from_cicp(cicp);
        if (color_space_or_error.is_error())
            return DecoderError::with_description(DecoderErrorCategory::Unknown, "Failed to create color space from CICP"sv);
        auto bitmap_or_error = Gfx::ImmutableBitmap::create_from_cv_pixel_buffer(frame->data[3], color_space_or_error.release_value());
        if (bitmap_or_error.is_error())
            return DecoderError::with_description(DecoderErrorCategory::Unknown, "Failed to create ImmutableBitmap from hardware frame"sv);
        return DECODER_TRY_ALLOC(try_make<VideoFrame>(timestamp, duration, size, 8, cicp, bitmap_or_error.release_value()));
    }
#endif

    auto pixel_format = static_cast<AVPixelFormat>(frame->format);
    bool semi_planar = is_semi_planar_yuv_format(pixel_format);
    if (!is_planar_yuv_format(pixel_format) && !semi_planar)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Unsupported pixel format {}", static_cast<int>(pixel_format));

    size_t bit_depth = [&] {
        switch (pixel_format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ444P:
        case AV_PIX_FMT_NV12:
            return 8;
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_P010:
            return 10;
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUV444P12:
            return 12;
        default:
            VERIFY_NOT_REACHED();
        }
    }();

    auto subsampling = [&]() -> Subsampling {
        switch (pixel_format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_P010:
            return { true, true };
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUVJ422P:
            return { true, false };
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_YUV444P12:
        case AV_PIX_FMT_YUVJ444P:
            return { false, false };
        default:
            VERIFY_NOT_REACHED();
        }
    }();

    auto gfx_size = size.to_type<int>();
    auto yuv_data = DECODER_TRY_ALLOC(Gfx::YUVData::create(gfx_size, bit_depth, subsampling, cicp));

    auto y_plane_size = size.to_type<size_t>();
    auto uv_plane_size = subsampling.subsampled_size(size).to_type<size_t>();
    auto component_size = bit_depth <= 8 ? 1 : 2;

    if (semi_planar) {
        auto const* y_source = frame->data[0];
        auto* y_dest = yuv_data->y_data().data();
        auto y_line_size = y_plane_size.width() * component_size;
        for (size_t row = 0; row < y_plane_size.height(); row++) {
            memcpy(y_dest, y_source, y_line_size);
            y_source += frame->linesize[0];
            y_dest += y_line_size;
        }

        auto const* uv_source = frame->data[1];
        auto* u_dest = yuv_data->u_data().data();
        auto* v_dest = yuv_data->v_data().data();
        for (size_t row = 0; row < uv_plane_size.height(); row++) {
            if (bit_depth <= 8) {
                for (size_t col = 0; col < uv_plane_size.width(); col++) {
                    u_dest[col] = uv_source[col * 2];
                    v_dest[col] = uv_source[col * 2 + 1];
                }
            } else {
                auto const* uv_src16 = reinterpret_cast<u16 const*>(uv_source);
                auto* u_dst16 = reinterpret_cast<u16*>(u_dest);
                auto* v_dst16 = reinterpret_cast<u16*>(v_dest);
                for (size_t col = 0; col < uv_plane_size.width(); col++) {
                    u_dst16[col] = uv_src16[col * 2];
                    v_dst16[col] = uv_src16[col * 2 + 1];
                }
            }
            uv_source += frame->linesize[1];
            u_dest += uv_plane_size.width() * component_size;
            v_dest += uv_plane_size.width() * component_size;
        }
    } else {
        Bytes buffers[] = { yuv_data->y_data(), yuv_data->u_data(), yuv_data->v_data() };
        Gfx::Size<size_t> plane_sizes[] = { y_plane_size, uv_plane_size, uv_plane_size };

        for (u32 plane = 0; plane < 3; plane++) {
            VERIFY(frame->linesize[plane] != 0);
            if (frame->linesize[plane] < 0)
                return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Reversed scanlines are not supported"sv);

            auto plane_size = plane_sizes[plane];
            auto const* source = frame->data[plane];
            VERIFY(source != nullptr);
            auto destination = buffers[plane];

            auto output_line_size = plane_size.width() * component_size;
            VERIFY(output_line_size <= static_cast<size_t>(frame->linesize[plane]));

            auto* dest_ptr = destination.data();
            for (size_t row = 0; row < plane_size.height(); row++) {
                memcpy(dest_ptr, source, output_line_size);
                source += frame->linesize[plane];
                dest_ptr += output_line_size;
            }
        }
    }

    auto bitmap = DECODER_TRY_ALLOC(Gfx::ImmutableBitmap::create_from_yuv(move(yuv_data)));
    return DECODER_TRY_ALLOC(try_make<VideoFrame>(timestamp, duration, size, bit_depth, cicp, move(bitmap)));
}

DecoderErrorOr<NonnullOwnPtr<VideoFrame>> FFmpegVideoDecoder::take_next_output()
{
    auto result = avcodec_receive_frame(m_codec_context, m_frame);

    switch (result) {
    case 0: {
        auto cicp = extract_cicp(m_frame, m_output_cicp);
        return materialize_frame(m_frame, cicp);
    }
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder has no frames available, send more input"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
    default:
        return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg codec encountered an unexpected error retrieving frames with code {:x}", result);
    }
}

void FFmpegVideoDecoder::flush()
{
    avcodec_flush_buffers(m_codec_context);
}

}

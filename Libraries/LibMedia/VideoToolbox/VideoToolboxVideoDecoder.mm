/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "VideoToolboxVideoDecoder.h"

#ifdef AK_OS_MACOS

#include <AK/ByteBuffer.h>
#include <AK/Queue.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Mutex.h>

#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

namespace Media::VideoToolbox {

struct VideoToolboxVideoDecoder::Impl {
    VTDecompressionSessionRef session { nullptr };
    CMFormatDescriptionRef format_description { nullptr };
    int width { 0 };
    int height { 0 };

    struct OutputFrame {
        CVPixelBufferRef pixel_buffer;
        AK::Duration timestamp;
        AK::Duration duration;
        CodingIndependentCodePoints cicp;
    };

    // Output queue populated by per-submission output handlers. Async submissions mean the
    // handler fires on an internal VT thread, so access is guarded by output_queue_mutex.
    Queue<OutputFrame> output_queue;
    Threading::Mutex output_queue_mutex;

    ~Impl()
    {
        while (!output_queue.is_empty())
            CVPixelBufferRelease(output_queue.dequeue().pixel_buffer);
        if (session) {
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
        }
        if (format_description)
            CFRelease(format_description);
    }

    DecoderErrorOr<void> ensure_session(int new_width, int new_height);
    DecoderErrorOr<void> submit_frame(ReadonlyBytes data, AK::Duration timestamp, VTDecodeFrameFlags flags, VTDecompressionOutputHandler output_handler);
};

DecoderErrorOr<NonnullOwnPtr<VideoToolboxVideoDecoder>> VideoToolboxVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes)
{
    if (codec_id != CodecID::VP9)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "VideoToolbox decoder only supports VP9, got {}", codec_id);

    // For VP9, we create the format description from the first frame, since we need the
    // dimensions. Defer session creation until receive_coded_data.
    auto impl = make<Impl>();
    return adopt_own(*new VideoToolboxVideoDecoder(move(impl)));
}

VideoToolboxVideoDecoder::VideoToolboxVideoDecoder(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

VideoToolboxVideoDecoder::~VideoToolboxVideoDecoder() = default;

DecoderErrorOr<void> VideoToolboxVideoDecoder::Impl::ensure_session(int new_width, int new_height)
{
    if (session && width == new_width && height == new_height)
        return {};

    if (session) {
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
        session = nullptr;
    }
    if (format_description) {
        CFRelease(format_description);
        format_description = nullptr;
    }

    width = new_width;
    height = new_height;

    // VP9 requires a vpcC (VPCodecConfigurationRecord) in the format description extensions.
    // 12-byte blob per https://www.webmproject.org/vp9/mp4/#vp-codec-configuration-box
    uint8_t vpcc[12] = {};
    vpcc[0] = 1;       // version
    vpcc[4] = 0;       // profile 0
    vpcc[5] = 51;      // level 5.1
    vpcc[6] = (8 << 4) // bit depth 8
            | (1 << 1); // chroma subsampling 4:2:0
    vpcc[7] = 1;       // color primaries: BT.709
    vpcc[8] = 1;       // transfer characteristics: BT.709
    vpcc[9] = 1;       // matrix coefficients: BT.709

    NSDictionary* atoms = @{
        @"vpcC": [NSData dataWithBytes:vpcc length:sizeof(vpcc)],
    };
    NSDictionary* format_extensions = @{
        (__bridge NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: atoms,
    };
    NSDictionary* decoder_specification = @{
        (__bridge NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: atoms,
        (__bridge NSString*)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES,
    };

    // VP9 is a "supplemental" decoder that must be registered before use.
    if (__builtin_available(macOS 11.0, *))
        VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);

    auto status = CMVideoFormatDescriptionCreate(
        kCFAllocatorDefault,
        kCMVideoCodecType_VP9,
        new_width,
        new_height,
        (__bridge CFDictionaryRef)format_extensions,
        &format_description);
    if (status != noErr)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to create VP9 format description: {}", status);

    status = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        format_description,
        (__bridge CFDictionaryRef)decoder_specification,
        nullptr, // destination image buffer attributes
        nullptr, // no session-level callback; per-call output handlers instead
        &session);
    if (status != noErr)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to create VT decompression session: {}", status);

    return {};
}

DecoderErrorOr<void> VideoToolboxVideoDecoder::Impl::submit_frame(ReadonlyBytes data, AK::Duration timestamp, VTDecodeFrameFlags flags, VTDecompressionOutputHandler output_handler)
{
    CMBlockBufferRef block_buffer = nullptr;
    auto status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        const_cast<u8*>(data.data()),
        data.size(),
        kCFAllocatorNull,
        nullptr,
        0,
        data.size(),
        0,
        &block_buffer);
    if (status != kCMBlockBufferNoErr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to create CMBlockBuffer"sv);

    CMSampleTimingInfo timing;
    timing.presentationTimeStamp = CMTimeMake(timestamp.to_microseconds(), 1'000'000);
    timing.duration = kCMTimeInvalid;
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample_buffer = nullptr;
    size_t sample_size = data.size();
    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        block_buffer,
        format_description,
        1,
        1,
        &timing,
        1,
        &sample_size,
        &sample_buffer);
    CFRelease(block_buffer);
    if (status != noErr)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to create CMSampleBuffer: {}", status);

    status = VTDecompressionSessionDecodeFrameWithOutputHandler(session, sample_buffer, flags, nullptr, output_handler);
    CFRelease(sample_buffer);
    if (status != noErr)
        return DecoderError::format(DecoderErrorCategory::Unknown, "VTDecompressionSessionDecodeFrameWithOutputHandler failed: {}", status);

    return {};
}

static Optional<Gfx::IntSize> parse_vp9_frame_size(ReadonlyBytes data)
{
    if (data.size() < 10)
        return {};

    // VP9 uncompressed header, bit-level parsing.
    u8 byte0 = data[0];
    int frame_marker = (byte0 >> 6) & 0x3;
    if (frame_marker != 0x2)
        return {};

    int profile = ((byte0 >> 4) & 0x1) | (((byte0 >> 5) & 0x1) << 1);
    int bit_offset = 4;

    if (profile == 3)
        bit_offset++; // reserved_zero

    int show_existing_frame = (byte0 >> (7 - bit_offset)) & 1;
    bit_offset++;
    if (show_existing_frame)
        return {};

    int frame_type = (byte0 >> (7 - bit_offset)) & 1;
    bit_offset++;
    if (frame_type != 0) // not a keyframe
        return {};

    // Skip show_frame and error_resilient
    bit_offset += 2;

    // Sync code at next byte boundary... actually, it's bit-aligned.
    // For profile 0 keyframe, bit_offset is now 8, so sync code starts at byte 1.
    int byte_pos = bit_offset / 8;
    int bit_pos = bit_offset % 8;

    // For simplicity, if not byte-aligned at this point, bail.
    if (bit_pos != 0)
        return {};

    if (byte_pos + 3 > static_cast<int>(data.size()))
        return {};

    // Verify sync code: 0x49, 0x83, 0x42
    if (data[byte_pos] != 0x49 || data[byte_pos + 1] != 0x83 || data[byte_pos + 2] != 0x42)
        return {};
    byte_pos += 3;

    // color_config for profile 0/1: 3 bits color_space + 1 bit color_range (if not CS_RGB)
    if (byte_pos >= static_cast<int>(data.size()))
        return {};

    int color_space = (data[byte_pos] >> 5) & 0x7;
    int config_bits = 3;
    if (color_space != 7) // not CS_RGB
        config_bits += 1; // color_range

    // frame_size: 16 bits width_minus_1 + 16 bits height_minus_1
    // Starting at bit offset: byte_pos*8 + config_bits
    int size_bit_offset = byte_pos * 8 + config_bits;
    int size_byte = size_bit_offset / 8;
    int size_bit = size_bit_offset % 8;

    if (size_byte + 4 >= static_cast<int>(data.size()))
        return {};

    // Read 16 bits for width, 16 bits for height from the bit stream.
    // This is easier with a bit reader, but let's just extract manually.
    u32 bits = 0;
    for (int i = 0; i < 5; i++)
        bits = (bits << 8) | data[size_byte + i];

    int shift = 40 - size_bit - 16;
    int width = ((bits >> shift) & 0xFFFF) + 1;
    shift -= 16;
    int height = ((bits >> shift) & 0xFFFF) + 1;

    return Gfx::IntSize { width, height };
}

DecoderErrorOr<void> VideoToolboxVideoDecoder::decode_for_reference(AK::Duration timestamp, AK::Duration, ReadonlyBytes coded_data)
{
    if (!m_impl->session) {
        auto size = parse_vp9_frame_size(coded_data);
        if (!size.has_value())
            return DecoderError::with_description(DecoderErrorCategory::Invalid, "First VP9 frame is not a keyframe or could not be parsed"sv);
        TRY(m_impl->ensure_session(size->width(), size->height()));
    }

    // Ref-only submissions don't fire the handler because of DoNotOutputFrame, but the API
    // requires a non-null block, so we pass an empty one.
    return m_impl->submit_frame(coded_data, timestamp,
        kVTDecodeFrame_DoNotOutputFrame | kVTDecodeFrame_EnableAsynchronousDecompression,
        ^(OSStatus, VTDecodeInfoFlags, CVImageBufferRef, CMTime, CMTime) { });
}

DecoderErrorOr<void> VideoToolboxVideoDecoder::decode_for_output(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data, CodingIndependentCodePoints const& container_cicp)
{
    if (!m_impl->session) {
        auto size = parse_vp9_frame_size(coded_data);
        if (!size.has_value())
            return DecoderError::with_description(DecoderErrorCategory::Invalid, "First VP9 frame is not a keyframe or could not be parsed"sv);
        TRY(m_impl->ensure_session(size->width(), size->height()));
    }

    // The output handler runs on a VT internal thread when the async decode completes.
    // It captures the per-submission metadata by value and enqueues the decoded buffer.
    auto* impl = m_impl.ptr();
    return m_impl->submit_frame(coded_data, timestamp,
        kVTDecodeFrame_EnableAsynchronousDecompression,
        ^(OSStatus status, VTDecodeInfoFlags, CVImageBufferRef imageBuffer, CMTime, CMTime) {
            if (status != noErr || !imageBuffer)
                return;
            CVPixelBufferRetain((CVPixelBufferRef)imageBuffer);
            Threading::MutexLocker locker { impl->output_queue_mutex };
            impl->output_queue.enqueue({
                .pixel_buffer = (CVPixelBufferRef)imageBuffer,
                .timestamp = timestamp,
                .duration = duration,
                .cicp = container_cicp,
            });
        });
}

DecoderErrorOr<NonnullOwnPtr<VideoFrame>> VideoToolboxVideoDecoder::take_next_output()
{
    auto try_dequeue = [&]() -> Optional<Impl::OutputFrame> {
        Threading::MutexLocker locker { m_impl->output_queue_mutex };
        if (m_impl->output_queue.is_empty())
            return {};
        return m_impl->output_queue.dequeue();
    };

    auto output_or_empty = try_dequeue();
    if (!output_or_empty.has_value()) {
        // Queue is empty; drain any in-flight async outputs and try once more.
        if (m_impl->session)
            VTDecompressionSessionWaitForAsynchronousFrames(m_impl->session);
        output_or_empty = try_dequeue();
        if (!output_or_empty.has_value())
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "No output frames available"sv);
    }
    auto output = output_or_empty.release_value();

    auto width = CVPixelBufferGetWidth(output.pixel_buffer);
    auto height = CVPixelBufferGetHeight(output.pixel_buffer);
    auto size = Gfx::Size<u32> { static_cast<u32>(width), static_cast<u32>(height) };

    auto color_space_or_error = Gfx::ColorSpace::from_cicp(output.cicp);
    if (color_space_or_error.is_error()) {
        CVPixelBufferRelease(output.pixel_buffer);
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "Failed to create color space"sv);
    }

    auto bitmap_or_error = Gfx::ImmutableBitmap::create_from_cv_pixel_buffer(output.pixel_buffer, color_space_or_error.release_value());
    CVPixelBufferRelease(output.pixel_buffer);

    if (bitmap_or_error.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "Failed to create ImmutableBitmap"sv);

    return DECODER_TRY_ALLOC(try_make<VideoFrame>(output.timestamp, output.duration, size, 8, output.cicp, bitmap_or_error.release_value()));
}

void VideoToolboxVideoDecoder::signal_end_of_stream()
{
    if (m_impl->session)
        VTDecompressionSessionFinishDelayedFrames(m_impl->session);
}

void VideoToolboxVideoDecoder::flush()
{
    if (m_impl->session)
        VTDecompressionSessionFinishDelayedFrames(m_impl->session);

    Threading::MutexLocker locker { m_impl->output_queue_mutex };
    while (!m_impl->output_queue.is_empty())
        CVPixelBufferRelease(m_impl->output_queue.dequeue().pixel_buffer);
}

}

#endif

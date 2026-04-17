/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/CodecID.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoDecoder.h>

#include "FFmpegForward.h"

struct AVBufferRef;

namespace Media::FFmpeg {

class MEDIA_API FFmpegVideoDecoder final : public VideoDecoder {
public:
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, AVBufferRef* hw_device_ctx);
    virtual ~FFmpegVideoDecoder() override;

    virtual DecoderErrorOr<void> decode_for_reference(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data) override;
    virtual DecoderErrorOr<void> decode_for_output(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data, CodingIndependentCodePoints const& container_cicp) override;
    virtual DecoderErrorOr<NonnullOwnPtr<VideoFrame>> take_next_output() override;

    virtual void signal_end_of_stream() override;
    virtual void flush() override;

private:
    DecoderErrorOr<void> send_packet(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data);
    DecoderErrorOr<NonnullOwnPtr<VideoFrame>> materialize_frame(AVFrame*, CodingIndependentCodePoints const& cicp);

    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
    AVBufferRef* m_hw_device_ctx;

    // Tracks the CICP to use for the next output frame we materialize from m_codec_context's
    // internal output queue. Set by decode_for_output.
    CodingIndependentCodePoints m_output_cicp {};
};

}

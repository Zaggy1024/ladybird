/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_MACOS

#include <AK/ByteBuffer.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoDecoder.h>

namespace Media::VideoToolbox {

class MEDIA_API VideoToolboxVideoDecoder final : public VideoDecoder {
public:
    static DecoderErrorOr<NonnullOwnPtr<VideoToolboxVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    virtual ~VideoToolboxVideoDecoder() override;

    virtual DecoderErrorOr<void> decode_for_reference(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data) override;
    virtual DecoderErrorOr<void> decode_for_output(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data, CodingIndependentCodePoints const& container_cicp) override;
    virtual DecoderErrorOr<NonnullOwnPtr<VideoFrame>> take_next_output() override;

    virtual void signal_end_of_stream() override;
    virtual void flush() override;

    // Public for the decompress callback.
    struct Impl;

    VideoToolboxVideoDecoder(NonnullOwnPtr<Impl>);

    NonnullOwnPtr<Impl> m_impl;
};

}

#endif

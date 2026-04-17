/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

#include "DecoderError.h"

namespace Media {

struct FrameMetadata {
    AK::Duration timestamp;
    AK::Duration duration;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() { }

    // Submit a coded frame for reference-state-only decoding. Any decoded frames are discarded.
    // Used by the seeking loop to advance the reference state through intermediate P-frames.
    virtual DecoderErrorOr<void> decode_for_reference(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data) = 0;

    // Submit a coded frame that will produce output. The decoded frame(s) are queued internally
    // and can be retrieved via take_next_output(). A single coded frame may produce multiple
    // outputs (e.g. VP9 superframes).
    virtual DecoderErrorOr<void> decode_for_output(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data, CodingIndependentCodePoints const& container_cicp) = 0;

    // Pop the next queued output frame. Returns NeedsMoreInput when the queue is empty.
    virtual DecoderErrorOr<NonnullOwnPtr<VideoFrame>> take_next_output() = 0;

    virtual void signal_end_of_stream() = 0;
    virtual void flush() = 0;
};

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecoderError.h>

namespace Audio {

// Time-stretching audio processor. Implementations are stateful: rate and pitch are
// set via set_rate/set_pitch and applied to ongoing processing; push_block feeds
// input, retrieve_block produces output. flush() resets the stream and arms preroll.
class TimeStretcher {
public:
    virtual ~TimeStretcher() = default;

    // Reset internal state. Subsequent push_block calls are expected to begin at
    // (target_start_timestamp - preroll_input_frames). If real input arrives later
    // (e.g. upstream clamped a negative seek to zero), the implementation silence-pads
    // to keep output aligned with target_start_timestamp.
    virtual void flush(AK::Duration target_start_timestamp) = 0;

    // Time-stretch rate (>1 faster, <1 slower). May be set at any time.
    virtual void set_rate(float rate) = 0;
    // Pitch as a frequency multiplier (1.0 = unchanged).
    virtual void set_pitch(float pitch) = 0;

    // Number of input-rate frames the implementation will consume as preroll/warmup
    // before emitting the first user-visible output frame. Valid after flush();
    // implementations without preroll return 0.
    virtual size_t preroll_input_frames() const = 0;

    virtual void push_block(Media::AudioBlock const& input) = 0;

    // Returns a block of output. DecoderErrorCategory::NeedsMoreInput when more
    // push_block calls are required before more output can be produced; or after
    // signal_end_of_stream() once all buffered output has been drained.
    virtual Media::DecoderErrorOr<Media::AudioBlock> retrieve_block() = 0;

    // Signals that no more push_block calls will follow on the current stream.
    // Subsequent retrieve_block calls drain any remaining buffered audio; once
    // empty they return NeedsMoreInput. flush() resets EOS state.
    virtual void signal_end_of_stream() = 0;
};

}

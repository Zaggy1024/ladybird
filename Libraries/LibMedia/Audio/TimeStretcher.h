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

    // Reset internal state and arm preroll. media_start_timestamp anchors the next
    // emitted block's media-time origin; output_start_frame_index anchors its
    // first_frame_index. The two anchors can differ — they accumulate independently
    // once any non-unit-rate emit has happened. Subsequent push_block calls are
    // expected to begin at (media_start_timestamp - preroll input frames). If real
    // input arrives later (e.g. upstream clamped a negative seek to zero), the
    // implementation silence-pads to keep output aligned with media_start_timestamp.
    // Returns the preroll input-frame count, valid until the next flush.
    virtual i64 flush_and_get_preroll(AK::Duration media_start_timestamp, i64 output_start_frame_index) = 0;

    // Time-stretch rate (>1 faster, <1 slower). May be set at any time.
    virtual void set_rate(float rate) = 0;
    // Pitch as a frequency multiplier (1.0 = unchanged).
    virtual void set_pitch(float pitch) = 0;

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

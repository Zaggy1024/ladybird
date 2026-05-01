/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/Audio/TimeStretcher.h>

namespace Audio {

class SonicTimeStretcher final : public TimeStretcher {
public:
    static ErrorOr<NonnullOwnPtr<TimeStretcher>> create(u32 sample_rate, u8 channel_count);
    virtual ~SonicTimeStretcher() override;

    virtual i64 flush_and_get_preroll(AK::Duration media_start_timestamp, i64 output_start_frame_index) override;
    virtual void set_rate(float rate) override;
    virtual void set_pitch(float pitch) override;
    virtual void push_block(Media::AudioBlock const& input) override;
    virtual Media::DecoderErrorOr<Media::AudioBlock> retrieve_block() override;
    virtual void signal_end_of_stream() override;

private:
    struct Impl;

    explicit SonicTimeStretcher(NonnullOwnPtr<Impl>);
    NonnullOwnPtr<Impl> m_impl;
};

}

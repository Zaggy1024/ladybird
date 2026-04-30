/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibMedia/Audio/TimeStretcher.h>

namespace Audio {

class BungeeTimeStretcher final : public TimeStretcher {
public:
    static NonnullOwnPtr<TimeStretcher> create(u32 sample_rate, u8 channel_count);
    virtual ~BungeeTimeStretcher() override;

    virtual void flush(AK::Duration target_start_timestamp) override;
    virtual void set_rate(float rate) override;
    virtual void set_pitch(float pitch) override;
    virtual size_t preroll_input_frames() const override;
    virtual void push_block(Media::AudioBlock const& input) override;
    virtual Media::DecoderErrorOr<Media::AudioBlock> retrieve_block() override;
    virtual void signal_end_of_stream() override;

private:
    struct Impl;

    explicit BungeeTimeStretcher(NonnullOwnPtr<Impl>);
    NonnullOwnPtr<Impl> m_impl;
};

}

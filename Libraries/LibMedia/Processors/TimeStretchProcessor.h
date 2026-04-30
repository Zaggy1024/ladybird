/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Audio/TimeStretcher.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Processors/AudioProcessor.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibThreading/Mutex.h>

namespace Media {

class MEDIA_API TimeStretchProcessor final : public AudioProcessor {
public:
    static ErrorOr<NonnullRefPtr<TimeStretchProcessor>> try_create();
    TimeStretchProcessor();
    virtual ~TimeStretchProcessor() override = default;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    virtual void seek(AK::Duration timestamp) override;

    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override;

    virtual void start() override;

    virtual PipelineStatus pull(AudioBlock& into) override;
    virtual void set_state_changed_handler(PipelineStateChangeHandler) override;

    virtual ErrorOr<void> set_stretch(float rate) override;

private:
    void ensure_stretcher_while_locked();

    mutable Threading::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    RefPtr<AudioProducer> m_input;

    OwnPtr<Audio::TimeStretcher> m_stretcher;
    Atomic<float, MemoryOrder::memory_order_relaxed> m_rate { 1.0f };

    i64 m_next_output_frame { 0 };
    bool m_started { false };
    bool m_upstream_eos_signalled { false };

    PipelineStateChangeHandler m_state_changed_handler;
};

}

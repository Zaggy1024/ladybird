/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibMedia/Track.h>
#include <LibThreading/Mutex.h>

namespace Media {

class MEDIA_API AudioMixer final : public AudioSink {
public:
    static ErrorOr<NonnullRefPtr<AudioMixer>> try_create();
    AudioMixer();
    virtual ~AudioMixer() override = default;

    virtual void set_producer(Track const&, RefPtr<DecodedAudioProducer> const&) override;
    virtual RefPtr<DecodedAudioProducer> producer(Track const&) const override;

    void set_sample_specification(Audio::SampleSpecification);
    Audio::SampleSpecification sample_specification() const;

    PipelineStatus pull(AudioBlock& into);

    // Combined: rewinds the mixer to a new sample position and discards any in-flight track blocks.
    // Must be invoked with no concurrent mix in progress.
    void reset_to_sample_position(i64 sample_position);

private:
    struct TrackMixingData {
        TrackMixingData(NonnullRefPtr<DecodedAudioProducer> const& producer)
            : producer(producer)
        {
        }

        NonnullRefPtr<DecodedAudioProducer> producer;
        AudioBlock current_block;
        i64 next_sample { 0 };
        PipelineStatus last_status { PipelineStatus::Pending };
    };

    mutable Threading::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    HashMap<Track, TrackMixingData> m_track_mixing_datas;
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_next_sample_to_write { 0 };
};

}

/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaTimeProvider.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibMedia/Sinks/AudioSink.h>

namespace Media {

class MEDIA_API AudioPlaybackSink final : public AudioSink, public MediaTimeProvider {
private:
    class OutputThreadData;

public:
    // Per-block media-time/output-frame snapshot recorded when the data callback
    // first touches a block. Used by current_time() to interpolate accurate media
    // time from the audio device's stream position, independent of any rate-multiplier
    // formula.
    struct BlockTiming {
        i64 first_frame_index;
        i64 frame_count;
        AK::Duration media_time_start;
        AK::Duration media_time_duration;

        i64 end_frame_index() const { return saturating_add(first_frame_index, frame_count); }
    };

    static ErrorOr<NonnullRefPtr<AudioPlaybackSink>> try_create(PipelineStateChangeHandler on_state_changed);
    AudioPlaybackSink(NonnullRefPtr<OutputThreadData>);
    virtual ~AudioPlaybackSink() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    void disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const&);
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    virtual AK::Duration current_time() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void seek(AK::Duration) override;
    virtual ErrorOr<void> set_playback_rate(float rate) override;

    void set_volume(double);

    Function<void(Error&&)> on_audio_output_error;

private:
    void create_playback_stream();

    Core::EventLoop& m_main_thread_event_loop;

    Audio::SampleSpecification m_sample_specification;

    bool m_started_creating_playback_stream { false };
    bool m_playing { false };
    double m_volume { 1 };

    AK::Duration m_anchor_stream_time { AK::Duration::zero() };
    i64 m_anchor_output_frame_index { 0 };

    Optional<AK::Duration> m_temporary_time;
    float m_playback_rate { 1.0f };

    mutable Optional<BlockTiming> m_current_block_timing;
    mutable AK::Duration m_minimum_media_time;

    NonnullRefPtr<OutputThreadData> m_output_thread_data;
};

}

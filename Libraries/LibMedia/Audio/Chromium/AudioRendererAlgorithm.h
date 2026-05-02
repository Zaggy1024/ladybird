// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported to Ladybird (BSD-2-Clause). Original copyright preserved per BSD-3.
// Source: chromium/media/filters/audio_renderer_algorithm.{h,cc}.
//
// This is a stripped-down port: only the WSOLA path remains. The passthrough,
// resampler, channel-mask, latency-hint, MediaLog, AudioBufferQueue, and
// bitstream branches from the original have been removed since the
// surrounding pipeline (TimeStretchProcessor / Ladybird audio pipeline)
// already handles the equivalents.

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>

namespace Audio::Chromium {

class AudioBus;

class AudioRendererAlgorithm {
public:
    AudioRendererAlgorithm(int sample_rate, int channel_count);
    ~AudioRendererAlgorithm();

    AudioRendererAlgorithm(AudioRendererAlgorithm const&) = delete;
    AudioRendererAlgorithm& operator=(AudioRendererAlgorithm const&) = delete;

    // Tries to fill `requested_frames` frames of `dest` (planar) starting at
    // `dest_offset` with WSOLA-stretched audio. Consumes input from the queue
    // as needed. Returns the number of frames actually written.
    int fill_buffer(AudioBus& dest, int dest_offset, int requested_frames, double playback_rate);

    // Append a buffer of input frames (planar). The buffer is consumed as
    // WSOLA proceeds. The implementation copies; the caller may release after.
    void enqueue_buffer(AudioBus const& buffer_in);

    // Clear the input queue and reset all WSOLA state.
    void flush_buffers();

    // Indicates that no more input will be enqueued. fill_buffer will then
    // produce the remaining buffered output before returning 0.
    void mark_end_of_stream();

    // Number of input frames currently buffered.
    int buffered_frames() const { return m_audio_buffer_frames; }

    int sample_rate() const { return m_sample_rate; }
    int channel_count() const { return m_channel_count; }
    int ola_window_size() const { return m_ola_window_size; }
    int ola_hop_size() const { return m_ola_hop_size; }

private:
    // One iteration of WSOLA. Returns true if it ran (i.e. enough input was
    // available); false otherwise.
    bool run_one_wsola_iteration(double playback_rate);

    // Copy completed OLA output frames to dest.
    int write_completed_frames_to(int requested_frames, int dest_offset, AudioBus& dest);

    // Fills `dest` from the input queue starting at `read_offset_frames` (in
    // frames since the start of the queue). Negative offsets zero-prepend.
    void peek_audio_with_zero_prepend(int read_offset_frames, AudioBus& dest);

    // Within `m_search_block`, find the block most similar to `m_target_block`
    // and write it into `m_optimal_block`.
    void get_optimal_block();

    // Are there enough buffered input frames to run WSOLA once?
    bool can_perform_wsola() const;

    // Is the target block fully within the search region? If so, the search
    // can be skipped.
    bool target_is_within_search_region() const;

    // Update m_output_time and m_search_block_index based on a hop change.
    void update_output_time(double playback_rate, double time_change);

    // After a WSOLA iteration, drop input frames that are no longer needed.
    void remove_old_input_frames(double playback_rate);

    // Append `num_frames` of zeros to the input queue (used for padding when
    // running short on input near a transition or near EOS).
    void append_silence_to_input(int num_frames);

    // Drop `num_frames` of input from the front of the queue.
    void drop_front_input_frames(int num_frames);

    int const m_sample_rate;
    int const m_channel_count;

    // Input queue: single planar AudioBus that grows as input is enqueued and
    // is rotated/dropped from the front as WSOLA consumes it.
    OwnPtr<AudioBus> m_audio_buffer;
    int m_audio_buffer_frames { 0 };

    // WSOLA parameters (computed from sample_rate at construction).
    int const m_ola_window_size;
    int const m_ola_hop_size;
    int const m_num_candidate_blocks;
    int const m_search_block_center_offset;

    // Running indices.
    double m_output_time { 0.0 };
    int m_search_block_index { 0 };
    int m_target_block_index { 0 };

    // Number of frames in m_wsola_output that overlap-add is completed for.
    int m_num_complete_frames { 0 };

    bool m_reached_end_of_stream { false };

    // Output staging area: holds completed OLA frames until consumed by
    // fill_buffer.
    OwnPtr<AudioBus> m_wsola_output;

    // Hann window used for OLA, and the transition window used in
    // get_optimal_block.
    Vector<float> m_ola_window;
    Vector<float> m_transition_window;

    // Auxiliary blocks (allocated once, reused per iteration).
    OwnPtr<AudioBus> m_optimal_block;
    OwnPtr<AudioBus> m_search_block;
    OwnPtr<AudioBus> m_target_block;
};

}

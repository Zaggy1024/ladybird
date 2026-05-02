// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported to Ladybird (BSD-2-Clause). Original copyright preserved per BSD-3.
// Source: chromium/media/filters/audio_renderer_algorithm.{h,cc}.
//
// Algorithm overview (preserved from Chromium):
//
// Waveform Similarity Overlap-and-add (WSOLA).
//
// One WSOLA iteration:
//
// 1) Extract `target_block` as input frames at indices
//    [target_block_index, target_block_index + ola_window_size).
//    target_block is the "natural" continuation of the output.
//
// 2) Extract `search_block` as input frames at indices
//    [search_block_index,
//     search_block_index + num_candidate_blocks + ola_window_size).
//
// 3) Find a block within search_block most similar to target_block. Let
//    optimal_index be its index and write the content to `optimal_block`.
//
// 4) Update:
//    optimal_block = transition_window * target_block
//                  + (1 - transition_window) * optimal_block.
//
// 5) Overlap-and-add `optimal_block` into `wsola_output`.
//
// 6) Update:
//    target_block_index = optimal_index + ola_window_size / 2.
//    output_index += ola_window_size / 2.
//    search_block_center_index = output_index * playback_rate.
//    search_block_index = search_block_center_index - search_block_center_offset.

#include <AK/Math.h>

#include "AudioBus.h"
#include "AudioRendererAlgorithm.h"
#include "WsolaInternals.h"

namespace Audio::Chromium {

namespace {

// Overlap-and-add window size in milliseconds.
constexpr double k_ola_window_size_ms = 20.0;
// Size of search interval in milliseconds. Search range is +- delta around
// output_index * playback_rate, so the full search interval is 2 * delta.
constexpr double k_wsola_search_interval_ms = 30.0;

int frames_for_milliseconds(int sample_rate, double milliseconds)
{
    return AK::round_to<int>((milliseconds / 1000.0) * sample_rate);
}

}

AudioRendererAlgorithm::AudioRendererAlgorithm(int sample_rate, int channel_count)
    : m_sample_rate(sample_rate)
    , m_channel_count(channel_count)
    // Make sure ola_window_size is even (Chromium does the same — 50% overlap
    // requires even).
    , m_ola_window_size(frames_for_milliseconds(sample_rate, k_ola_window_size_ms)
        + (frames_for_milliseconds(sample_rate, k_ola_window_size_ms) & 1))
    , m_ola_hop_size(m_ola_window_size / 2)
    , m_num_candidate_blocks(frames_for_milliseconds(sample_rate, k_wsola_search_interval_ms))
    // Offset of the center of the search block to the center of the first
    // (left-most) candidate block. The center of a candidate block is at
    // ola_window_size/2 - 1; the center of the search block is at
    // num_candidate_blocks/2 + (ola_window_size/2 - 1).
    , m_search_block_center_offset((m_num_candidate_blocks / 2) + ((m_ola_window_size / 2) - 1))
{
    VERIFY(sample_rate > 0);
    VERIFY(channel_count > 0);

    m_audio_buffer = AudioBus::create(channel_count, 0);

    m_ola_window.resize(m_ola_window_size);
    WsolaInternals::get_periodic_hanning_window(m_ola_window.span());

    m_transition_window.resize(static_cast<size_t>(m_ola_window_size) * 2);
    WsolaInternals::get_periodic_hanning_window(m_transition_window.span());

    m_wsola_output = AudioBus::create(channel_count, m_ola_window_size + m_ola_hop_size);
    m_wsola_output->zero();

    m_optimal_block = AudioBus::create(channel_count, m_ola_window_size);
    m_search_block = AudioBus::create(channel_count, m_num_candidate_blocks + (m_ola_window_size - 1));
    m_target_block = AudioBus::create(channel_count, m_ola_window_size);
}

AudioRendererAlgorithm::~AudioRendererAlgorithm() = default;

void AudioRendererAlgorithm::flush_buffers()
{
    // Re-create the input queue empty.
    m_audio_buffer = AudioBus::create(m_channel_count, 0);
    m_audio_buffer_frames = 0;

    m_output_time = 0.0;
    m_search_block_index = 0;
    m_target_block_index = 0;
    m_num_complete_frames = 0;
    m_reached_end_of_stream = false;

    if (m_wsola_output)
        m_wsola_output->zero();
}

void AudioRendererAlgorithm::enqueue_buffer(AudioBus const& buffer_in)
{
    VERIFY(buffer_in.channels() == m_channel_count);
    int const incoming_frames = buffer_in.frames();
    if (incoming_frames == 0)
        return;

    int const new_total_frames = m_audio_buffer_frames + incoming_frames;
    auto resized_buffer = AudioBus::create(m_channel_count, new_total_frames);

    // Copy existing contents.
    if (m_audio_buffer_frames > 0)
        m_audio_buffer->copy_partial_frames_to(0, m_audio_buffer_frames, 0, *resized_buffer);

    // Append new contents.
    buffer_in.copy_partial_frames_to(0, incoming_frames, m_audio_buffer_frames, *resized_buffer);

    m_audio_buffer = move(resized_buffer);
    m_audio_buffer_frames = new_total_frames;
}

void AudioRendererAlgorithm::mark_end_of_stream()
{
    m_reached_end_of_stream = true;
}

void AudioRendererAlgorithm::append_silence_to_input(int num_frames)
{
    VERIFY(num_frames >= 0);
    if (num_frames == 0)
        return;
    int const new_total_frames = m_audio_buffer_frames + num_frames;
    auto resized_buffer = AudioBus::create(m_channel_count, new_total_frames);
    if (m_audio_buffer_frames > 0)
        m_audio_buffer->copy_partial_frames_to(0, m_audio_buffer_frames, 0, *resized_buffer);
    resized_buffer->zero_frames_partial(m_audio_buffer_frames, num_frames);
    m_audio_buffer = move(resized_buffer);
    m_audio_buffer_frames = new_total_frames;
}

void AudioRendererAlgorithm::drop_front_input_frames(int num_frames)
{
    VERIFY(num_frames >= 0);
    VERIFY(num_frames <= m_audio_buffer_frames);
    if (num_frames == 0)
        return;
    int const new_total_frames = m_audio_buffer_frames - num_frames;
    auto resized_buffer = AudioBus::create(m_channel_count, new_total_frames);
    if (new_total_frames > 0)
        m_audio_buffer->copy_partial_frames_to(num_frames, new_total_frames, 0, *resized_buffer);
    m_audio_buffer = move(resized_buffer);
    m_audio_buffer_frames = new_total_frames;
}

int AudioRendererAlgorithm::fill_buffer(AudioBus& dest, int dest_offset, int requested_frames, double playback_rate)
{
    if (playback_rate == 0.0)
        return 0;
    VERIFY(playback_rate > 0.0);
    VERIFY(dest.channels() == m_channel_count);

    int rendered_frames = 0;
    do {
        rendered_frames += write_completed_frames_to(
            requested_frames - rendered_frames,
            dest_offset + rendered_frames, dest);
    } while (rendered_frames < requested_frames && run_one_wsola_iteration(playback_rate));
    return rendered_frames;
}

bool AudioRendererAlgorithm::can_perform_wsola() const
{
    int const search_block_size = m_num_candidate_blocks + (m_ola_window_size - 1);
    int const frames = m_audio_buffer_frames;
    return m_target_block_index + m_ola_window_size <= frames
        && m_search_block_index + search_block_size <= frames;
}

bool AudioRendererAlgorithm::target_is_within_search_region() const
{
    int const search_block_size = m_num_candidate_blocks + (m_ola_window_size - 1);
    return m_target_block_index >= m_search_block_index
        && m_target_block_index + m_ola_window_size <= m_search_block_index + search_block_size;
}

void AudioRendererAlgorithm::peek_audio_with_zero_prepend(int read_offset_frames, AudioBus& dest)
{
    int const dest_frame_count = dest.frames();
    VERIFY(read_offset_frames + dest_frame_count <= m_audio_buffer_frames);

    int write_offset = 0;
    int frames_to_read = dest_frame_count;
    if (read_offset_frames < 0) {
        int num_zero_frames_appended = min(-read_offset_frames, frames_to_read);
        read_offset_frames = 0;
        frames_to_read -= num_zero_frames_appended;
        write_offset = num_zero_frames_appended;
        dest.zero_frames(num_zero_frames_appended);
    }
    if (frames_to_read > 0)
        m_audio_buffer->copy_partial_frames_to(read_offset_frames, frames_to_read, write_offset, dest);
}

void AudioRendererAlgorithm::get_optimal_block()
{
    int optimal_index = 0;

    // An interval around the last optimal block excluded from the search.
    // This reduces "buzzy" sound. 160 frames is heuristic in Chromium.
    constexpr int exclude_interval_length_frames = 160;

    if (target_is_within_search_region()) {
        optimal_index = m_target_block_index;
        peek_audio_with_zero_prepend(optimal_index, *m_optimal_block);
    } else {
        peek_audio_with_zero_prepend(m_target_block_index, *m_target_block);
        peek_audio_with_zero_prepend(m_search_block_index, *m_search_block);
        int last_optimal = m_target_block_index - m_ola_hop_size - m_search_block_index;
        WsolaInternals::Interval exclude_interval {
            static_cast<size_t>(last_optimal - (exclude_interval_length_frames / 2)),
            static_cast<size_t>(last_optimal + (exclude_interval_length_frames / 2)),
        };

        // optimal_index is in frames relative to the start of search_block.
        optimal_index = static_cast<int>(WsolaInternals::optimal_index(*m_search_block, *m_target_block, exclude_interval));

        // Translate w.r.t. the start of m_audio_buffer and extract the optimal block.
        optimal_index += m_search_block_index;
        peek_audio_with_zero_prepend(optimal_index, *m_optimal_block);

        // Make a transition from target to optimal for a smoother result.
        // The transition window is 2x the OLA window length, used as a weighting:
        // weight 1 at the start (target dominates), 0 at the end (optimal dominates).
        for (int channel_index = 0; channel_index < m_channel_count; ++channel_index) {
            auto opt_channel = m_optimal_block->channel(channel_index);
            auto target_channel = m_target_block->channel(channel_index);
            for (int n = 0; n < m_ola_window_size; ++n) {
                opt_channel[n] = (opt_channel[n] * m_transition_window[n])
                    + (target_channel[n] * m_transition_window[m_ola_window_size + n]);
            }
        }
    }

    // Next target is one hop ahead of the current optimal.
    m_target_block_index = optimal_index + m_ola_hop_size;
}

bool AudioRendererAlgorithm::run_one_wsola_iteration(double playback_rate)
{
    if (!can_perform_wsola())
        return false;

    get_optimal_block();

    // Overlap-and-add.
    auto const ola_hop_size = static_cast<size_t>(m_ola_hop_size);
    for (int channel_index = 0; channel_index < m_channel_count; ++channel_index) {
        auto opt_channel = m_optimal_block->channel(channel_index);
        auto output_channel = m_wsola_output->channel(channel_index).slice(static_cast<size_t>(m_num_complete_frames));

        // First half: weighted overlap-add with rising window on optimal_block,
        // falling window on existing wsola_output (which is the previous block's
        // tail laid down ola_hop_size frames ago).
        for (size_t n = 0; n < ola_hop_size; ++n) {
            output_channel[n] = (output_channel[n] * m_ola_window[ola_hop_size + n])
                + (opt_channel[n] * m_ola_window[n]);
        }

        // Second half: copy directly. It will be overlap-added with the next
        // block's first half on the following iteration.
        for (size_t n = 0; n < ola_hop_size; ++n)
            output_channel[ola_hop_size + n] = opt_channel[ola_hop_size + n];
    }

    m_num_complete_frames += m_ola_hop_size;
    update_output_time(playback_rate, m_ola_hop_size);
    remove_old_input_frames(playback_rate);
    return true;
}

void AudioRendererAlgorithm::update_output_time(double playback_rate, double time_change)
{
    m_output_time += time_change;
    int const search_block_center_index = AK::round_to<int>(m_output_time * playback_rate);
    m_search_block_index = search_block_center_index - m_search_block_center_offset;
}

void AudioRendererAlgorithm::remove_old_input_frames(double playback_rate)
{
    int const earliest_used_index = min(m_target_block_index, m_search_block_index);
    if (earliest_used_index <= 0)
        return; // Nothing to remove.

    drop_front_input_frames(earliest_used_index);
    m_target_block_index -= earliest_used_index;

    double const output_time_change = static_cast<double>(earliest_used_index) / playback_rate;
    VERIFY(m_output_time >= output_time_change);
    update_output_time(playback_rate, -output_time_change);
}

int AudioRendererAlgorithm::write_completed_frames_to(int requested_frames, int dest_offset, AudioBus& dest)
{
    int const rendered_frames = min(m_num_complete_frames, requested_frames);
    if (rendered_frames == 0)
        return 0;

    m_wsola_output->copy_partial_frames_to(0, rendered_frames, dest_offset, dest);

    // Shift remaining wsola_output content to the front.
    int const frames_to_move = m_wsola_output->frames() - rendered_frames;
    for (int channel_index = 0; channel_index < m_channel_count; ++channel_index) {
        auto channel_data = m_wsola_output->channel(channel_index);
        for (int i = 0; i < frames_to_move; ++i)
            channel_data[i] = channel_data[i + rendered_frames];
    }
    m_num_complete_frames -= rendered_frames;
    return rendered_frames;
}

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Minimal AudioBus replacement for the WSOLA port — a planar float buffer
// (one Vector<float> per channel) with the subset of the Chromium AudioBus
// API used by audio_renderer_algorithm and wsola_internals.

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>

namespace Audio::Chromium {

class AudioBus {
public:
    static NonnullOwnPtr<AudioBus> create(int channel_count, int frame_count);

    int channels() const { return static_cast<int>(m_channels.size()); }
    int frames() const { return m_frame_count; }

    Span<float> channel(int channel_index) { return m_channels[channel_index].span(); }
    ReadonlySpan<float> channel(int channel_index) const { return m_channels[channel_index].span(); }

    void zero();
    void zero_frames(int frame_count_to_zero);
    void zero_frames_partial(int starting_frame, int frame_count_to_zero);

    // Copies frame_count frames starting at source_offset to dest at dest_offset.
    void copy_partial_frames_to(int source_offset, int frame_count, int dest_offset, AudioBus& dest) const;

private:
    AudioBus(int channel_count, int frame_count);

    int m_frame_count { 0 };
    Vector<Vector<float>> m_channels;
};

}

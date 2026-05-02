/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypedTransfer.h>
#include <algorithm>

#include "AudioBus.h"

namespace Audio::Chromium {

AudioBus::AudioBus(int channel_count, int frame_count)
    : m_frame_count(frame_count)
{
    VERIFY(channel_count > 0);
    VERIFY(frame_count >= 0);
    m_channels.resize(static_cast<size_t>(channel_count));
    for (auto& channel_data : m_channels)
        channel_data.resize(static_cast<size_t>(frame_count));
}

NonnullOwnPtr<AudioBus> AudioBus::create(int channel_count, int frame_count)
{
    return adopt_own(*new AudioBus(channel_count, frame_count));
}

void AudioBus::zero()
{
    for (auto& channel_data : m_channels)
        std::fill(channel_data.begin(), channel_data.end(), 0.0f);
}

void AudioBus::zero_frames(int frame_count_to_zero)
{
    zero_frames_partial(0, frame_count_to_zero);
}

void AudioBus::zero_frames_partial(int starting_frame, int frame_count_to_zero)
{
    VERIFY(starting_frame >= 0);
    VERIFY(frame_count_to_zero >= 0);
    VERIFY(starting_frame + frame_count_to_zero <= m_frame_count);
    for (auto& channel_data : m_channels) {
        auto* base = channel_data.data() + starting_frame;
        std::fill(base, base + frame_count_to_zero, 0.0f);
    }
}

void AudioBus::copy_partial_frames_to(int source_offset, int frame_count, int dest_offset, AudioBus& dest) const
{
    VERIFY(channels() == dest.channels());
    VERIFY(source_offset >= 0 && source_offset + frame_count <= m_frame_count);
    VERIFY(dest_offset >= 0 && dest_offset + frame_count <= dest.frames());
    for (int channel_index = 0; channel_index < channels(); ++channel_index) {
        AK::TypedTransfer<float>::copy(
            dest.m_channels[channel_index].data() + dest_offset,
            m_channels[channel_index].data() + source_offset,
            static_cast<size_t>(frame_count));
    }
}

}

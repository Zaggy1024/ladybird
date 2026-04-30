/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/SaturatingMath.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/SampleSpecification.h>

namespace Media {

class AudioBlock {
public:
    using Data = Vector<float>;

    Audio::SampleSpecification const& sample_specification() const { return m_sample_specification; }

    AK::Duration media_time_start() const { return m_media_time_start; }
    AK::Duration media_time_duration() const { return m_media_time_duration; }
    AK::Duration media_time_end() const { return m_media_time_start + m_media_time_duration; }

    i64 first_frame_index() const { return m_first_frame_index; }
    i64 end_frame_index() const { return saturating_add(m_first_frame_index, AK::clamp_to<i64>(frame_count())); }

    Span<float> data() { return m_data; }
    ReadonlySpan<float> data() const { return m_data; }

    void clear()
    {
        m_sample_specification = {};
        m_media_time_start = {};
        m_media_time_duration = {};
        m_first_frame_index = 0;
        m_data.clear_with_capacity();
    }
    template<typename Callback>
    void emplace(Audio::SampleSpecification sample_specification, AK::Duration media_time_start, Callback data_callback)
    {
        VERIFY(sample_specification.is_valid());
        m_sample_specification = sample_specification;
        m_media_time_start = media_time_start;
        m_first_frame_index = media_time_start.to_time_units(1, sample_rate());
        data_callback(m_data);
        recalculate_media_duration_from_frame_count();
    }
    template<typename Callback>
    void emplace(Audio::SampleSpecification sample_specification, i64 first_frame_index, Callback data_callback)
    {
        VERIFY(sample_specification.is_valid());
        m_sample_specification = sample_specification;
        m_first_frame_index = first_frame_index;
        m_media_time_start = AK::Duration::from_time_units(first_frame_index, 1, sample_rate());
        data_callback(m_data);
        recalculate_media_duration_from_frame_count();
    }
    void trim(size_t frame_count)
    {
        m_data.resize_and_keep_capacity(frame_count * channel_count());
        recalculate_media_duration_from_frame_count();
    }
    u32 sample_rate() const
    {
        return sample_specification().sample_rate();
    }
    void set_media_time_start(AK::Duration media_time_start)
    {
        VERIFY(!is_empty());
        m_media_time_start = media_time_start;
    }
    void set_media_time_duration(AK::Duration media_time_duration)
    {
        VERIFY(!is_empty());
        VERIFY(!media_time_duration.is_negative());
        m_media_time_duration = media_time_duration;
    }
    void set_first_frame_index(i64 first_frame_index)
    {
        VERIFY(!is_empty());
        m_first_frame_index = first_frame_index;
    }
    bool is_empty() const
    {
        return !sample_specification().is_valid();
    }
    size_t sample_count() const
    {
        return data().size();
    }
    u8 channel_count() const
    {
        return sample_specification().channel_map().channel_count();
    }
    size_t frame_count() const
    {
        return sample_count() / channel_count();
    }

private:
    void recalculate_media_duration_from_frame_count()
    {
        m_media_time_duration = AK::Duration::from_time_units(static_cast<i64>(frame_count()), 1, sample_rate());
    }

    Audio::SampleSpecification m_sample_specification;
    AK::Duration m_media_time_start;
    AK::Duration m_media_time_duration;
    i64 m_first_frame_index { 0 };
    Data m_data;
};

}

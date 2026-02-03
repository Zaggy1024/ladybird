/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#track-buffers
class TrackBuffer final {
public:
    TrackBuffer();
    ~TrackBuffer();

    // https://w3c.github.io/media-source/#last-decode-timestamp
    Optional<double> last_decode_timestamp() const { return m_last_decode_timestamp; }
    void set_last_decode_timestamp(double timestamp) { m_last_decode_timestamp = timestamp; }
    void unset_last_decode_timestamp() { m_last_decode_timestamp = {}; }

    // https://w3c.github.io/media-source/#last-frame-duration
    Optional<double> last_frame_duration() const { return m_last_frame_duration; }
    void set_last_frame_duration(double duration) { m_last_frame_duration = duration; }
    void unset_last_frame_duration() { m_last_frame_duration = {}; }

    // https://w3c.github.io/media-source/#highest-end-timestamp
    Optional<double> highest_end_timestamp() const { return m_highest_end_timestamp; }
    void set_highest_end_timestamp(double timestamp) { m_highest_end_timestamp = timestamp; }
    void unset_highest_end_timestamp() { m_highest_end_timestamp = {}; }

    // https://w3c.github.io/media-source/#need-RAP-flag
    bool need_random_access_point_flag() const { return m_need_random_access_point_flag; }
    void set_need_random_access_point_flag(bool flag) { m_need_random_access_point_flag = flag; }

    // https://w3c.github.io/media-source/#track-buffer-ranges
    // FIXME: Return a TimeRanges-like structure
    void track_buffer_ranges() const;

    void add_coded_frame(double presentation_timestamp, double decode_timestamp, double frame_duration);

    void remove_coded_frames_in_range(double start, double end);

    void remove_all_coded_frames();

    bool has_coded_frame_at_presentation_timestamp(double timestamp) const;

    Optional<double> next_random_access_point_timestamp_after(double timestamp) const;

private:
    // https://w3c.github.io/media-source/#last-decode-timestamp
    Optional<double> m_last_decode_timestamp;

    // https://w3c.github.io/media-source/#last-frame-duration
    Optional<double> m_last_frame_duration;

    // https://w3c.github.io/media-source/#highest-end-timestamp
    Optional<double> m_highest_end_timestamp;

    // https://w3c.github.io/media-source/#need-RAP-flag
    bool m_need_random_access_point_flag { true };
};

}

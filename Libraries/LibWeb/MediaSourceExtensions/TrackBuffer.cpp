/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/MediaSourceExtensions/TrackBuffer.h>

namespace Web::MediaSourceExtensions {

TrackBuffer::TrackBuffer(Media::Track const& track)
    : m_track(track)
{
}

TrackBuffer::~TrackBuffer()
{
    dbgln("[oo]");
}

// https://w3c.github.io/media-source/#track-buffer-ranges
void TrackBuffer::track_buffer_ranges() const
{
    // FIXME: Return the presentation time ranges occupied by the coded frames currently stored in the track buffer.
}

void TrackBuffer::add_coded_frame(double presentation_timestamp, double decode_timestamp, double frame_duration)
{
    (void)presentation_timestamp;
    (void)decode_timestamp;
    (void)frame_duration;
    // FIXME: Add the coded frame with the given timestamps and duration to the track buffer.
}

void TrackBuffer::remove_coded_frames_in_range(double start, double end)
{
    (void)start;
    (void)end;
    // FIXME: Remove all coded frames from the track buffer that have a presentation timestamp
    //        greater than or equal to start and less than end.
}

void TrackBuffer::remove_all_coded_frames()
{
    // FIXME: Remove all coded frames from the track buffer.
}

bool TrackBuffer::has_coded_frame_at_presentation_timestamp(double timestamp) const
{
    (void)timestamp;
    // FIXME: Check if a coded frame exists at the given presentation timestamp.
    return false;
}

Optional<double> TrackBuffer::next_random_access_point_timestamp_after(double timestamp) const
{
    (void)timestamp;
    // FIXME: Find the next random access point timestamp after the given timestamp.
    return {};
}

}

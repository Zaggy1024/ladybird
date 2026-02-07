/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Types.h>
#include <LibMedia/DecoderError.h>

namespace Web::MediaSourceExtensions {

enum class SegmentType : u8 {
    Incomplete,
    Unknown,
    InitializationSegment,
    MediaSegment,
};

class ByteStreamParser {
public:
    virtual ~ByteStreamParser() = default;

    virtual size_t check_for_bytes_to_skip(ReadonlyBytes) = 0;
    virtual SegmentType sniff_segment_type(ReadonlyBytes) = 0;
    virtual Media::DecoderErrorOr<size_t> parse_initialization_segment(ReadonlyBytes) = 0;
    virtual Vector<Media::Track> const& video_tracks() = 0;
    virtual Vector<Media::Track> const& audio_tracks() = 0;
    virtual Vector<Media::Track> const& text_tracks() = 0;
};

}

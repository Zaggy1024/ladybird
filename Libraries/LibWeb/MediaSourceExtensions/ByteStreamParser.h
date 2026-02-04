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
    size_t check_for_bytes_to_skip(ReadonlyBytes);
    SegmentType sniff_segment_type(ReadonlyBytes);
    Media::DecoderErrorOr<size_t> parse_initialization_segment(ReadonlyBytes);
};

}

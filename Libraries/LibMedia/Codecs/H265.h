/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media::Codecs {

class H265 {
public:
    struct Parameters {
        Array<u8, 6> constraint_indicator_flags;
        u32 profile_compatibility_flags;
        u8 profile_space;
        u8 profile_idc;
        u8 level_idc;
        bool tier_flag;

        bool operator==(Parameters const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
    static MEDIA_API Optional<Parameters> parse_configuration_record(ReadonlyBytes);
};

}

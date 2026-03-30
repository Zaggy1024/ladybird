/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConstantBitrateContainerNavigator.h"

namespace Media {

TimeRanges ConstantBitrateContainerNavigator::buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges, AK::Duration total_duration) const
{
    if (byte_ranges.is_empty())
        return {};

    TimeRanges ranges;

    for (auto const& byte_range : byte_ranges) {
        if (byte_range.end <= m_first_sample_position)
            continue;

        auto data_start = (byte_range.start > m_first_sample_position) ? byte_range.start - m_first_sample_position : 0;
        auto data_end = byte_range.end - m_first_sample_position;

        auto time_start = AK::Duration::from_milliseconds(static_cast<i64>(data_start * 1000 / m_bytes_per_second));
        auto time_end = AK::Duration::from_milliseconds(static_cast<i64>(data_end * 1000 / m_bytes_per_second));

        ranges.add_range(max(AK::Duration::zero(), time_start), min(total_duration, time_end));
    }

    return ranges;
}

}

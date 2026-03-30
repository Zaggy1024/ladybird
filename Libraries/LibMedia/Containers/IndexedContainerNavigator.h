/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Vector.h>

#include "ContainerNavigator.h"

namespace Media {

class IndexedContainerNavigator final : public ContainerNavigator {
public:
    struct IndexEntry {
        size_t position;
        AK::Duration timestamp;
    };

    IndexedContainerNavigator(Vector<IndexEntry>&& entries)
        : m_entries(move(entries))
    {
    }

    TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges, AK::Duration total_duration) const override;

private:
    size_t lower_bound(size_t target) const;

    Vector<IndexEntry> m_entries;
};

}

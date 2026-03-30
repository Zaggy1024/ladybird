/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>

#include "ContainerNavigator.h"

namespace Media {

template<typename Derived>
class ScanningContainerNavigator : public ContainerNavigator {
public:
    TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges, AK::Duration total_duration) const override
    {
        auto const& self = *static_cast<Derived const*>(this);

        // Reconcile incoming byte ranges with cached ranges in lockstep. Both are sorted by position.
        size_t cached_index = 0;
        size_t byte_index = 0;

        while (byte_index < byte_ranges.size()) {
            auto const& byte_range = byte_ranges[byte_index];

            if (cached_index < m_cached_ranges.size()) {
                auto& cached = m_cached_ranges[cached_index];

                // If the cached range doesn't have the same start byte as the incoming range, then we need to update
                // the start time of the cached range if possible. If that's not possible, then we discard the cached
                // range if it's earlier, or insert a new one if it's later.
                if (cached.byte_start != byte_range.start) {
                    if (cached.byte_end == byte_range.end) {
                        cached.byte_start = byte_range.start;
                        cached.time_start = Unscanned {};
                        if (cached.time_end.template has<NotFound>())
                            cached.time_end = Unscanned {};
                        cached_index++;
                        byte_index++;
                        continue;
                    }

                    if (cached.byte_end < byte_range.end) {
                        cached = CachedRange { .byte_start = byte_range.start, .byte_end = byte_range.end };
                        cached_index++;
                        byte_index++;
                        continue;
                    }

                    if (cached.byte_start < byte_range.start) {
                        m_cached_ranges.remove(cached_index);
                    } else {
                        m_cached_ranges.insert(cached_index, CachedRange { .byte_start = byte_range.start, .byte_end = byte_range.end });
                        cached_index++;
                        byte_index++;
                    }
                    continue;
                }

                // If we're here, the cached range's start matches the incoming one, so we only need to update the end.
                if (cached.byte_end != byte_range.end) {
                    cached.byte_end = byte_range.end;
                    if (cached.time_start.template has<NotFound>())
                        cached.time_start = Unscanned {};
                    cached.time_end = Unscanned {};
                }

                cached_index++;
                byte_index++;
            } else {
                // We have new byte ranges on the end with no equivalent cached ranges, append them.
                m_cached_ranges.append(CachedRange { .byte_start = byte_range.start, .byte_end = byte_range.end });
                cached_index++;
                byte_index++;
            }
        }

        // Remove any leftover cached ranges past the end.
        if (cached_index < m_cached_ranges.size())
            m_cached_ranges.remove(cached_index, m_cached_ranges.size() - cached_index);

        // Scan any unscanned boundaries and build the result.
        TimeRanges result;
        for (auto& cached : m_cached_ranges) {
            if (cached.time_start.template has<Unscanned>()) {
                auto timestamp = self.scan_forward_for_timestamp(cached.byte_start, cached.byte_end);
                cached.time_start = timestamp.has_value() ? ScanResult(*timestamp) : ScanResult(NotFound {});
            }
            if (cached.time_end.template has<Unscanned>()) {
                auto timestamp = self.scan_backward_for_end_timestamp(cached.byte_start, cached.byte_end);
                cached.time_end = timestamp.has_value() ? ScanResult(*timestamp) : ScanResult(NotFound {});
            }

            if (!cached.time_start.template has<AK::Duration>() || !cached.time_end.template has<AK::Duration>())
                continue;

            auto time_start = cached.time_start.template get<AK::Duration>();
            auto time_end = cached.time_end.template get<AK::Duration>();
            if (time_start >= time_end)
                continue;

            result.add_range(max(AK::Duration::zero(), time_start), min(total_duration, time_end));
        }
        return result;
    }

private:
    struct Unscanned { };
    struct NotFound { };
    using ScanResult = Variant<Unscanned, NotFound, AK::Duration>;

    struct CachedRange {
        size_t byte_start { 0 };
        size_t byte_end { 0 };
        ScanResult time_start { Unscanned {} };
        ScanResult time_end { Unscanned {} };
    };

    mutable Vector<CachedRange> m_cached_ranges;
};

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>

#include "ScanningContainerNavigator.h"

namespace Media {

class FLACNavigator final : public ScanningContainerNavigator<FLACNavigator> {
public:
    static OwnPtr<FLACNavigator> create(ReadonlyBytes first_frame, NonnullRefPtr<MediaStreamCursor> cursor, u32 sample_rate);

    Optional<AK::Duration> scan_forward_for_timestamp(size_t search_start, size_t search_end) const;
    Optional<AK::Duration> scan_backward_for_end_timestamp(size_t search_start, size_t search_end) const;

private:
    struct FrameInfo {
        u64 sample_number;
        u16 block_size;
    };

    FLACNavigator(NonnullRefPtr<MediaStreamCursor> cursor, u32 sample_rate, u16 sync_code, u16 fixed_block_size)
        : m_cursor(move(cursor))
        , m_sample_rate(sample_rate)
        , m_sync_code(sync_code)
        , m_fixed_block_size(fixed_block_size)
    {
    }

    Optional<FrameInfo> find_first_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const;
    Optional<FrameInfo> find_last_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const;
    static Optional<FrameInfo> try_parse_frame_header(MediaStreamCursor& cursor, u16 sync_code, u16 fixed_block_size);

    AK::Duration sample_to_duration(u64 sample) const;

    NonnullRefPtr<MediaStreamCursor> m_cursor;
    u32 m_sample_rate;
    u16 m_sync_code;
    u16 m_fixed_block_size;
};

}

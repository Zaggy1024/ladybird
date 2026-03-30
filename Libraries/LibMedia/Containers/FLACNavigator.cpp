/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <AK/Endian.h>

#include <LibMedia/ReadonlyBytesCursor.h>

#include "FLACNavigator.h"

namespace Media {

static bool read_exact(MediaStreamCursor& cursor, Bytes buffer)
{
    auto result = cursor.read_into(buffer);
    return !result.is_error() && result.value() == buffer.size();
}

template<Integral T, Integral V>
static bool read(MediaStreamCursor& cursor, V& value)
{
    T read_value = 0;
    bool result = read_exact(cursor, { &read_value, sizeof(read_value) });
    value = AK::convert_between_host_and_big_endian(read_value);
    return result;
}

static bool skip(MediaStreamCursor& cursor, i64 bytes)
{
    auto result = cursor.seek(bytes, SeekMode::FromCurrentPosition);
    return !result.is_error();
}

template<Integral T>
static bool seek(MediaStreamCursor& cursor, T position)
{
    if (position > NumericLimits<i64>::max())
        return false;

    auto result = cursor.seek(static_cast<i64>(position), SeekMode::SetPosition);
    return !result.is_error();
}

[[maybe_unused]] static bool read_coded_number2(MediaStreamCursor& cursor, u64& value)
{
    u8 first_byte;
    if (!read_exact(cursor, { &first_byte, 1 }))
        return {};

    int extra_bytes;

    if ((first_byte & 0x80) == 0) {
        value = first_byte;
        extra_bytes = 0;
    } else if ((first_byte & 0xE0) == 0xC0) {
        value = first_byte & 0x1F;
        extra_bytes = 1;
    } else if ((first_byte & 0xF0) == 0xE0) {
        value = first_byte & 0x0F;
        extra_bytes = 2;
    } else if ((first_byte & 0xF8) == 0xF0) {
        value = first_byte & 0x07;
        extra_bytes = 3;
    } else if ((first_byte & 0xFC) == 0xF8) {
        value = first_byte & 0x03;
        extra_bytes = 4;
    } else if ((first_byte & 0xFE) == 0xFC) {
        value = first_byte & 0x01;
        extra_bytes = 5;
    } else if (first_byte == 0xFE) {
        value = 0;
        extra_bytes = 6;
    } else {
        return false;
    }

    for (int i = 0; i < extra_bytes; i++) {
        u8 continuation;
        if (!read_exact(cursor, { &continuation, 1 }))
            return {};
        if ((continuation & 0xC0) != 0x80)
            return {};
        value = (value << 6) | (continuation & 0x3F);
    }

    return true;
}

[[maybe_unused]] static bool read_coded_number(MediaStreamCursor& cursor, u64& value)
{
    if (!read<u8>(cursor, value))
        return false;
    auto length = count_leading_zeroes_safe(static_cast<u8>(~value));
    if (length == 0)
        return true;
    if (length == 1)
        return false;
    value &= (1 << (8 - (length + 1))) - 1;

    while (--length > 0) {
        u8 continuation_byte = 0;
        if (!read<u8>(cursor, continuation_byte))
            return false;
        if (continuation_byte >> 6 != 0b10)
            return false;
        value <<= 6;
        value |= continuation_byte & 0b111111;
    }

    return true;
}

static bool verify_header_crc(MediaStreamCursor& cursor, size_t header_start)
{
    constexpr auto lookup_table = [] {
        Array<u8, 256> result;
        for (size_t i = 0; i < result.size(); i++) {
            u8 value = i;
            for (auto j = 0; j < 8; j++)
                value = (value << 1) ^ (value & 0x80 ? 0x07 : 0);
            result[i] = value;
        }
        return result;
    }();

    auto header_end = cursor.position();
    VERIFY(header_end > header_start);
    auto header_size = header_end - header_start;
    Array<u8, 16> buffer;
    if (header_size > buffer.size())
        return false;

    u8 crc = 0;
    if (!read<u8>(cursor, crc))
        return false;

    if (!seek(cursor, header_start))
        return false;
    auto header_data = buffer.span().trim(header_size);
    if (!read_exact(cursor, header_data))
        return false;
    if (!skip(cursor, 1))
        return false;
    u8 actual_crc = 0;
    for (auto const& byte : header_data)
        actual_crc = lookup_table[actual_crc ^ byte];
    return crc == actual_crc;
}

OwnPtr<FLACNavigator> FLACNavigator::create(ReadonlyBytes first_frame, NonnullRefPtr<MediaStreamCursor> cursor, u32 sample_rate)
{
    if (first_frame.size() < 2)
        return {};
    u16 sync_code = (static_cast<u16>(first_frame[0]) << 8) | first_frame[1];
    if ((sync_code >> 1) != 0b111111111111100)
        return {};

    bool is_fixed_blocksize = (sync_code & 1) == 0;

    auto frame_cursor = make_ref_counted<ReadonlyBytesCursor>(first_frame);
    auto navigator = adopt_own(*new (nothrow) FLACNavigator(move(cursor), sample_rate, sync_code, 0));
    auto frame_info = try_parse_frame_header(frame_cursor, sync_code, 0);
    if (!frame_info.has_value())
        return {};

    if (is_fixed_blocksize)
        navigator->m_fixed_block_size = frame_info->block_size;

    return navigator;
}

Optional<FLACNavigator::FrameInfo> FLACNavigator::try_parse_frame_header(MediaStreamCursor& cursor, u16 sync_code, u16 fixed_block_size)
{
    auto header_start = cursor.position();

    Array<u8, 4> header;
    if (!read_exact(cursor, header))
        return {};

    u16 maybe_sync_code = (static_cast<u16>(header[0]) << 8) | header[1];
    if (maybe_sync_code != sync_code)
        return {};

    bool variable_block_size = (sync_code & 1) != 0;
    u8 block_size_bits = (header[2] >> 4) & 0x0F;
    u8 sample_rate_bits = header[2] & 0x0F;
    u8 channels_bits = (header[3] >> 4) & 0x0F;
    u8 bit_depth_bits = (header[3] >> 1) & 0x07;
    u8 reserved_bit = header[3] & 0x01;

    if (reserved_bit != 0)
        return {};

    u64 coded_number = 0;
    if (!read_coded_number(cursor, coded_number))
        return {};

    // Resolve block size if it was deferred to the end of the header.
    u16 block_size = 0;

    // https://www.rfc-editor.org/rfc/rfc9639.html#name-block-size-bits
    if (block_size_bits == 0b0000)
        return {};
    if (block_size_bits == 0b0001) {
        block_size = 192;
    } else if (block_size_bits <= 0b0101) {
        block_size = 144 * (1 << block_size_bits);
    } else if (block_size_bits == 0b0110) {
        if (!read<u8>(cursor, block_size))
            return {};
        block_size++;
    } else if (block_size_bits == 0b0111) {
        if (!read<u16>(cursor, block_size))
            return {};
        block_size++;
    } else if (block_size_bits >= 0b1000) {
        block_size = 1 << block_size_bits;
    }

    // https://www.rfc-editor.org/rfc/rfc9639.html#name-sample-rate-bits
    if (sample_rate_bits == 0b1100 && !skip(cursor, 1))
        return {};
    if ((sample_rate_bits == 0b1101 || sample_rate_bits == 0b1110) && !skip(cursor, 2))
        return {};
    if (sample_rate_bits == 0b1111)
        return {};

    // https://www.rfc-editor.org/rfc/rfc9639.html#name-channels-bits
    if (channels_bits >= 0b1011)
        return {};

    // https://www.rfc-editor.org/rfc/rfc9639.html#name-bit-depth-bits
    if (bit_depth_bits == 0b011)
        return {};

    // https://www.rfc-editor.org/rfc/rfc9639.html#name-frame-header-crc
    if (!verify_header_crc(cursor, header_start))
        return {};

    u64 sample_number = coded_number;
    if (!variable_block_size)
        sample_number *= fixed_block_size;

    return FrameInfo { sample_number, block_size };
}

static constexpr size_t SCAN_CHUNK_SIZE = 4096;

Optional<FLACNavigator::FrameInfo> FLACNavigator::find_first_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const
{
    VERIFY(search_start <= search_end);

    Array<u8, SCAN_CHUNK_SIZE> chunk;
    auto chunk_start = search_start;

    while (chunk_start < search_end) {
        if (!seek(cursor, chunk_start))
            return {};
        auto read_result = cursor.read_into(chunk);
        if (read_result.is_error())
            return {};
        auto bytes_read = read_result.value();
        if (bytes_read < 2)
            return {};

        for (size_t i = 0; i + 1 < bytes_read; i++) {
            if (chunk[i] != 0xFF)
                continue;
            u16 maybe_sync_code = (static_cast<u16>(chunk[i]) << 8) | chunk[i + 1];
            if (maybe_sync_code != m_sync_code)
                continue;

            auto sync_position = chunk_start + i;
            if (!seek(cursor, sync_position))
                return {};
            auto frame_info = try_parse_frame_header(cursor, m_sync_code, m_fixed_block_size);
            if (frame_info.has_value())
                return frame_info;
        }

        chunk_start += bytes_read - 1;
    }
    return {};
}

Optional<FLACNavigator::FrameInfo> FLACNavigator::find_last_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const
{
    VERIFY(search_start <= search_end);

    Array<u8, SCAN_CHUNK_SIZE> chunk;
    auto chunk_end = search_end;

    while (chunk_end > search_start) {
        auto chunk_start = chunk_end > SCAN_CHUNK_SIZE ? chunk_end - SCAN_CHUNK_SIZE : 0;
        chunk_start = max(chunk_start, search_start);
        VERIFY(chunk_start <= chunk_end);
        auto chunk_size = chunk_end - chunk_start;
        if (chunk_size < 2)
            return {};

        if (!seek(cursor, chunk_start))
            return {};
        if (!read_exact(cursor, chunk.span().trim(chunk_size)))
            return {};

        for (size_t i = chunk_size - 1; i-- > 0;) {
            u16 maybe_sync_code = (static_cast<u16>(chunk[i]) << 8) | chunk[i + 1];
            if (maybe_sync_code != m_sync_code)
                continue;

            auto sync_position = chunk_start + i;
            if (!seek(cursor, sync_position))
                return {};
            auto frame_info = try_parse_frame_header(cursor, m_sync_code, m_fixed_block_size);
            if (frame_info.has_value())
                return frame_info;
        }

        chunk_end = chunk_start + 1;
    }

    return {};
}

AK::Duration FLACNavigator::sample_to_duration(u64 sample) const
{
    return AK::Duration::from_time_units(static_cast<i64>(sample), 1, m_sample_rate);
}

Optional<AK::Duration> FLACNavigator::scan_forward_for_timestamp(size_t search_start, size_t search_end) const
{
    auto first = find_first_frame(*m_cursor, search_start, search_end);
    if (!first.has_value())
        return {};
    return sample_to_duration(first->sample_number);
}

Optional<AK::Duration> FLACNavigator::scan_backward_for_end_timestamp(size_t search_start, size_t search_end) const
{
    auto last = find_last_frame(*m_cursor, search_start, search_end);
    if (!last.has_value())
        return {};
    return sample_to_duration(last->sample_number + last->block_size);
}

}

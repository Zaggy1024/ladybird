/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OggNavigator.h"

#include <AK/Endian.h>
#include <AK/Math.h>
#include <LibMedia/Codecs/Opus.h>
#include <LibMedia/ReadonlyBytesCursor.h>

namespace Media {

static constexpr StringView CAPTURE_PATTERN = "OggS"sv;
static constexpr size_t CAPTURE_PATTERN_SIZE = CAPTURE_PATTERN.length();
static constexpr size_t PAGE_HEADER_SIZE = 27;
static constexpr size_t MAX_PAGE_SEGMENTS = 255;
static constexpr size_t CHUNK_SIZE = 4096;

static constexpr u8 CONTINUED_PACKET_FLAG = 1 << 0;
static constexpr i64 UNSET_GRANULE_POSITION = -1;

OggNavigator::OggNavigator(NonnullRefPtr<MediaStreamCursor> cursor, CodecData codec_data)
    : m_cursor(move(cursor))
    , m_codec_data(move(codec_data))
{
}

OwnPtr<OggNavigator> OggNavigator::create(ReadonlyBytes first_packet, NonnullRefPtr<MediaStreamCursor> cursor, CodecID codec_id, u32 time_base_numerator, u32 time_base_denominator, u32 sample_rate, ReadonlyBytes codec_initialization_data)
{
    VERIFY(time_base_numerator > 0);
    VERIFY(time_base_denominator > 0);

    switch (codec_id) {
    case CodecID::Opus:
        return adopt_own(*new (nothrow) OggNavigator(move(cursor), Opus { time_base_numerator, time_base_denominator }));
    case CodecID::FLAC: {
        if (sample_rate == 0 || first_packet.size() < 2)
            return {};

        u16 sync_code = (static_cast<u16>(first_packet[0]) << 8) | first_packet[1];
        if (!Codecs::FLAC::is_sync_code(sync_code))
            return {};

        auto frame_cursor = make_ref_counted<ReadonlyBytesCursor>(first_packet);
        auto frame_info = Codecs::FLAC::parse_frame_header(frame_cursor, sync_code, 0);
        if (!frame_info.has_value())
            return {};

        u16 fixed_block_size = 0;
        if ((sync_code & 1) == 0)
            fixed_block_size = frame_info->block_size;

        return adopt_own(*new (nothrow) OggNavigator(move(cursor), FLAC { sync_code, fixed_block_size, sample_rate }));
    }
    case CodecID::Vorbis: {
        if (sample_rate == 0)
            return {};
        if (codec_initialization_data.is_empty())
            return {};

        auto parser = Codecs::Vorbis::Parser::create(codec_initialization_data);
        if (!parser.has_value())
            return {};

        return adopt_own(*new (nothrow) OggNavigator(move(cursor), Vorbis { parser.release_value(), sample_rate }));
    }
    default:
        return {};
    }
}

AK::Duration OggNavigator::granule_to_time(i64 granule_position) const
{
    return m_codec_data.visit(
        [&](Opus const& codec) {
            return AK::Duration::from_time_units(max<i64>(0, granule_position), codec.time_base_numerator, codec.time_base_denominator);
        },
        [&](FLAC const& codec) {
            return AK::Duration::from_time_units(max<i64>(0, granule_position), 1, codec.sample_rate);
        },
        [&](Vorbis const& codec) {
            return AK::Duration::from_time_units(max<i64>(0, granule_position), 1, codec.sample_rate);
        });
}

void OggNavigator::reset_packet_parser_state() const
{
    m_codec_data.visit(
        [](Opus const&) {},
        [](FLAC const&) {},
        [](Vorbis const& codec) {
            codec.parser.reset();
        });
}

Optional<u64> OggNavigator::read_packet_duration_in_granules(size_t packet_size) const
{
    auto packet_start = m_cursor->position();
    Optional<u64> duration;

    duration = m_codec_data.visit(
        [&](Opus const&) -> Optional<u64> {
            auto parsed_duration = Codecs::Opus::parse_frame_duration_in_samples(*m_cursor, packet_size);
            if (parsed_duration.is_error())
                return {};
            return parsed_duration.release_value();
        },
        [&](FLAC const& codec) -> Optional<u64> {
            auto frame = Codecs::FLAC::parse_frame_header(*m_cursor, codec.sync_code, codec.fixed_block_size);
            if (!frame.has_value())
                return {};
            return frame->block_size;
        },
        [&](Vorbis const& codec) -> Optional<u64> {
            Array<u8, 1> packet_prefix;
            if (packet_size < packet_prefix.size())
                return {};
            if (m_cursor->read_until_filled(packet_prefix).is_error())
                return {};
            auto packet_duration = codec.parser.parse_packet_duration_in_samples(packet_prefix);
            if (!packet_duration.has_value())
                return {};
            return packet_duration.value();
        });
    if (!duration.has_value())
        return {};

    auto bytes_consumed = m_cursor->position() - packet_start;
    if (bytes_consumed > packet_size)
        return {};
    if (m_cursor->skip(AK::clamp_to<i64>(packet_size - bytes_consumed)).is_error())
        return {};

    return duration;
}

static bool is_capture_pattern(ReadonlyBytes bytes)
{
    return bytes.starts_with(CAPTURE_PATTERN.bytes());
}

static bool append_bytes_from_cursor(ByteBuffer& buffer, MediaStreamCursor& cursor, size_t byte_count)
{
    auto original_size = buffer.size();
    if (buffer.try_resize(original_size + byte_count).is_error())
        return false;
    return !cursor.read_until_filled(buffer.span().slice(original_size)).is_error();
}

Optional<u64> OggNavigator::read_packet_duration_in_granules(ReadonlyBytes packet) const
{
    auto packet_cursor = make_ref_counted<ReadonlyBytesCursor>(packet);
    return m_codec_data.visit(
        [&](Opus const&) -> Optional<u64> {
            auto parsed_duration = Codecs::Opus::parse_frame_duration_in_samples(packet_cursor, packet.size());
            if (parsed_duration.is_error())
                return {};
            return parsed_duration.release_value();
        },
        [&](FLAC const& codec) -> Optional<u64> {
            auto frame = Codecs::FLAC::parse_frame_header(packet_cursor, codec.sync_code, codec.fixed_block_size);
            if (!frame.has_value())
                return {};
            return frame->block_size;
        },
        [&](Vorbis const& codec) -> Optional<u64> {
            auto packet_duration = codec.parser.parse_packet_duration_in_samples(packet);
            if (!packet_duration.has_value())
                return {};
            return packet_duration.value();
        });
}

Optional<OggNavigator::PageScanResult> OggNavigator::parse_page_at(size_t page_start, size_t search_end, ByteBuffer* continued_packet) const
{
    if (page_start + PAGE_HEADER_SIZE > search_end)
        return {};
    if (m_cursor->seek_to_position(page_start).is_error())
        return {};

    Array<u8, PAGE_HEADER_SIZE> header;
    if (m_cursor->read_until_filled(header).is_error())
        return {};
    if (!is_capture_pattern(header))
        return {};
    auto version = header[4];
    if (version != 0)
        return {};
    auto header_type_flag = header[5];
    bool begins_with_continued_packet = (header_type_flag & CONTINUED_PACKET_FLAG) != 0;

    auto granule_position_bytes = header.span().slice(6, sizeof(i64));
    auto granule_position = granule_position_bytes.reinterpret<i64>()[0];
    granule_position = AK::convert_between_host_and_little_endian(granule_position);

    auto page_segments = header[26];
    if (page_start + PAGE_HEADER_SIZE + page_segments > search_end)
        return {};

    Array<u8, MAX_PAGE_SEGMENTS> segment_table_buffer;
    auto segment_table = segment_table_buffer.span().trim(page_segments);
    if (m_cursor->read_until_filled(segment_table).is_error())
        return {};

    size_t payload_size = 0;
    for (auto segment_size : segment_table)
        payload_size += segment_size;
    auto payload_start = page_start + PAGE_HEADER_SIZE + page_segments;
    auto payload_end = payload_start + payload_size;
    auto available_payload_size = min(payload_size, search_end - payload_start);

    u64 parsed_packet_duration_in_granules { 0 };
    size_t packet_size = 0;
    size_t packet_start = 0;
    bool packet_started_before_page = begins_with_continued_packet;
    for (auto segment_size : segment_table) {
        packet_size += segment_size;
        if (segment_size == 255)
            continue;

        if (packet_start + packet_size > available_payload_size)
            break;

        if (packet_started_before_page) {
            if (continued_packet && !continued_packet->is_empty()) {
                if (!append_bytes_from_cursor(*continued_packet, *m_cursor, packet_size))
                    return {};

                auto packet_duration = read_packet_duration_in_granules(continued_packet->span());
                continued_packet->clear();
                if (!packet_duration.has_value())
                    return {};
                parsed_packet_duration_in_granules += packet_duration.value();
            } else if (m_cursor->skip(AK::clamp_to<i64>(packet_size)).is_error()) {
                return {};
            }
            packet_started_before_page = false;
            packet_start += packet_size;
            packet_size = 0;
            continue;
        }

        auto packet_duration = read_packet_duration_in_granules(packet_size);
        if (!packet_duration.has_value())
            return {};
        parsed_packet_duration_in_granules += packet_duration.value();
        packet_start += packet_size;
        packet_size = 0;
    }

    if (continued_packet && packet_size > 0 && (!packet_started_before_page || !continued_packet->is_empty())) {
        if (m_cursor->seek_to_position(payload_start + packet_start).is_error())
            return {};
        if (!append_bytes_from_cursor(*continued_packet, *m_cursor, min(packet_size, available_payload_size - packet_start)))
            return {};
    }

    if (payload_end > search_end)
        return PageScanResult { {}, parsed_packet_duration_in_granules, page_start, search_end };
    if (granule_position == UNSET_GRANULE_POSITION)
        return PageScanResult { {}, parsed_packet_duration_in_granules, page_start, payload_end };

    return PageScanResult { granule_position, parsed_packet_duration_in_granules, page_start, payload_end };
}

Optional<AK::Duration> OggNavigator::find_start_timestamp(size_t search_start, size_t search_end) const
{
    VERIFY(search_start <= search_end);

    Array<u8, CHUNK_SIZE> chunk;
    auto chunk_start = search_start;

    while (chunk_start + CAPTURE_PATTERN_SIZE <= search_end) {
        auto chunk_size = min(chunk.size(), search_end - chunk_start);
        if (m_cursor->seek_to_position(chunk_start).is_error())
            return {};
        auto read_result = m_cursor->read_into(chunk.span().trim(chunk_size));
        if (read_result.is_error())
            return {};
        auto bytes_read = read_result.value();
        if (bytes_read < CAPTURE_PATTERN_SIZE)
            return {};

        for (size_t i = 0; i + CAPTURE_PATTERN_SIZE <= bytes_read; i++) {
            if (!is_capture_pattern(chunk.span().slice(i)))
                continue;
            reset_packet_parser_state();
            auto page = parse_page_at(chunk_start + i, search_end);
            if (!page.has_value())
                continue;
            if (!page->granule_position.has_value())
                continue;
            auto parsed_packet_duration_in_granules = static_cast<i64>(page->parsed_packet_duration_in_granules);
            if (page->granule_position.value() <= parsed_packet_duration_in_granules)
                return AK::Duration::zero();
            return granule_to_time(page->granule_position.value() - parsed_packet_duration_in_granules);
        }

        chunk_start += bytes_read - (CAPTURE_PATTERN_SIZE - 1);
    }

    return {};
}

Optional<OggNavigator::PageScanResult> OggNavigator::find_last_page_with_valid_granule_position(size_t search_start, size_t search_end) const
{
    VERIFY(search_start <= search_end);

    auto chunk_end = search_end;
    Array<u8, CHUNK_SIZE> chunk;
    while (chunk_end >= search_start + CAPTURE_PATTERN_SIZE) {
        auto chunk_start = chunk_end > chunk.size() ? chunk_end - chunk.size() : 0;
        chunk_start = max(chunk_start, search_start);
        auto chunk_size = chunk_end - chunk_start;

        if (m_cursor->seek_to_position(chunk_start).is_error())
            return {};
        if (m_cursor->read_until_filled(chunk.span().trim(chunk_size)).is_error())
            return {};

        for (size_t i = chunk_size - CAPTURE_PATTERN_SIZE + 1; i-- > 0;) {
            if (!is_capture_pattern(chunk.span().slice(i)))
                continue;
            reset_packet_parser_state();
            auto page = parse_page_at(chunk_start + i, search_end);
            if (page.has_value() && page->granule_position.has_value())
                return page;
        }

        if (chunk_start == search_start)
            break;
        chunk_end = chunk_start + CAPTURE_PATTERN_SIZE - 1;
    }

    return {};
}

Optional<AK::Duration> OggNavigator::find_end_timestamp(size_t search_start, size_t search_end) const
{
    auto last_complete_page = find_last_page_with_valid_granule_position(search_start, search_end);
    if (!last_complete_page.has_value())
        return {};

    ByteBuffer continued_packet;
    reset_packet_parser_state();
    auto page = parse_page_at(last_complete_page->byte_start, search_end, &continued_packet);
    if (!page.has_value() || !page->granule_position.has_value())
        return granule_to_time(last_complete_page->granule_position.value());

    auto end_granule_position = page->granule_position.value();
    auto page_start = page->byte_end;
    while (page_start + CAPTURE_PATTERN_SIZE <= search_end) {
        page = parse_page_at(page_start, search_end, &continued_packet);
        if (!page.has_value())
            break;
        if (page->granule_position.has_value())
            end_granule_position = page->granule_position.value();
        else
            end_granule_position += static_cast<i64>(page->parsed_packet_duration_in_granules);
        if (page->byte_end >= search_end)
            break;
        page_start = page->byte_end;
    }

    return granule_to_time(end_granule_position);
}

void OggNavigator::on_cached_range_changed(OggCachedRange& cached, CachedRangeChange change) const
{
    bool rescan_start = has_flag(change, CachedRangeChange::Start);
    bool rescan_end = has_flag(change, CachedRangeChange::End);

    if (rescan_start && !cached.time_end.has_value())
        rescan_end = true;
    if (rescan_end && !cached.time_start.has_value())
        rescan_start = true;

    if (rescan_start)
        cached.time_start = find_start_timestamp(cached.byte_start, cached.byte_end);
    if (rescan_end)
        cached.time_end = find_end_timestamp(cached.byte_start, cached.byte_end);
}

void OggNavigator::append_time_range(OggCachedRange const& cached_range, TimeRanges& to)
{
    if (!cached_range.time_start.has_value() || !cached_range.time_end.has_value())
        return;

    auto time_start = max(cached_range.time_start.value(), AK::Duration::zero());
    auto time_end = cached_range.time_end.value();
    if (time_start >= time_end)
        return;

    to.add_range(time_start, time_end);
}

}

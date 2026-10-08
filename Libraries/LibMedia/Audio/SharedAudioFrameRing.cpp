/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/Audio/SharedAudioFrameRing.h>

namespace Audio {

size_t SharedAudioFrameRing::buffer_size_for(u64 frame_capacity, u32 channel_count)
{
    return SAMPLES_OFFSET + frame_capacity * channel_count * sizeof(float);
}

ErrorOr<SharedAudioFrameRing> SharedAudioFrameRing::create(u32 sample_rate, u32 channel_count, size_t minimum_frame_capacity)
{
    VERIFY(sample_rate > 0);
    VERIFY(channel_count > 0);
    VERIFY(minimum_frame_capacity > 0);

    auto frame_capacity = round_up_to_a_power_of_two(minimum_frame_capacity);
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(buffer_size_for(frame_capacity, channel_count)));
    auto* header = new (buffer.data<void>()) Header;
    header->ring.channel_count = channel_count;
    header->ring.frame_capacity = frame_capacity;
    header->sample_rate = sample_rate;
    return SharedAudioFrameRing { move(buffer), header };
}

ErrorOr<SharedAudioFrameRing> SharedAudioFrameRing::create(Core::AnonymousBuffer buffer)
{
    if (!buffer.is_valid() || buffer.size() < sizeof(Header))
        return Error::from_string_literal("Audio frame ring buffer is too small for its header");
    TRY(buffer.validate_backing_size());

    auto* header = reinterpret_cast<Header*>(buffer.data<void>());
    if (header->sample_rate == 0 || header->ring.channel_count == 0)
        return Error::from_string_literal("Audio frame ring has an invalid sample specification");
    if (header->ring.frame_capacity == 0 || !is_power_of_two(header->ring.frame_capacity))
        return Error::from_string_literal("Audio frame ring has an invalid frame capacity");
    if (buffer.size() < buffer_size_for(header->ring.frame_capacity, header->ring.channel_count))
        return Error::from_string_literal("Audio frame ring buffer is too small for its frame capacity");
    return SharedAudioFrameRing { move(buffer), header };
}

SharedAudioFrameRing::SharedAudioFrameRing(Core::AnonymousBuffer buffer, Header* header)
    : m_buffer(move(buffer))
    , m_shared_header(header)
{
    auto* samples = reinterpret_cast<float*>(m_buffer.data<u8>() + SAMPLES_OFFSET);
    set_storage(header->ring, { samples, static_cast<size_t>(header->ring.frame_capacity * header->ring.channel_count) });
}

static u64 frames_in_nanoseconds(i64 nanoseconds, u32 sample_rate)
{
    auto whole_seconds = nanoseconds / 1'000'000'000;
    auto remainder_ns = nanoseconds % 1'000'000'000;
    return static_cast<u64>(whole_seconds * sample_rate + (remainder_ns * sample_rate) / 1'000'000'000);
}

u64 SharedAudioFrameRing::estimate_frames_played(i64 monotonic_ns) const
{
    auto previous_estimate = m_last_estimated_frames_played.load(AK::MemoryOrder::memory_order_relaxed);
    auto anchor = m_shared_header->consumption.read();
    if (!anchor.has_value())
        return previous_estimate;

    u64 estimate = anchor->frames_consumed;
    if (monotonic_ns < anchor->played_at_ns) {
        // The device still holds the tail of what it consumed.
        auto pending_frames = frames_in_nanoseconds(anchor->played_at_ns - monotonic_ns, m_shared_header->sample_rate);
        estimate = pending_frames >= estimate ? 0 : estimate - pending_frames;
    } else if (anchor->playing) {
        estimate += frames_in_nanoseconds(monotonic_ns - anchor->played_at_ns, m_shared_header->sample_rate);
    }
    estimate = min(estimate, frames_written());

    // Only ever raise the published estimate, whichever thread gets there first.
    while (estimate > previous_estimate) {
        if (m_last_estimated_frames_played.compare_exchange_strong(previous_estimate, estimate, AK::MemoryOrder::memory_order_relaxed))
            return estimate;
    }
    return previous_estimate;
}

size_t SharedAudioFrameRing::pop_mixing_into(Span<float> mix, float gain)
{
    auto channel_count = m_header->channel_count;
    auto frame_capacity = m_header->frame_capacity;
    VERIFY(mix.size() % channel_count == 0);

    auto head = m_header->head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_acquire);
    auto frames_to_read = min<u64>(mix.size() / channel_count, tail - head);
    if (frames_to_read == 0)
        return 0;

    auto ring_mask = frame_capacity * channel_count - 1;
    auto ring_index = (head & (frame_capacity - 1)) * channel_count;
    for (size_t i = 0; i < frames_to_read * channel_count; i++) {
        mix[i] += m_samples[ring_index] * gain;
        ring_index = (ring_index + 1) & ring_mask;
    }

    m_header->head.store(head + frames_to_read, AK::MemoryOrder::memory_order_release);
    return frames_to_read;
}

void SharedAudioFrameRing::discard_all()
{
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_acquire);
    m_header->head.store(tail, AK::MemoryOrder::memory_order_release);
}

void SharedAudioFrameRing::publish_consumption(i64 played_at_ns, bool playing)
{
    PlaybackStreamConsumptionAnchor anchor {
        .frames_consumed = m_header->head.load(AK::MemoryOrder::memory_order_relaxed),
        .played_at_ns = played_at_ns,
        .playing = playing,
    };
    m_shared_header->consumption.store(anchor);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Audio::SharedAudioFrameRing const& ring)
{
    return encoder.encode(ring.buffer());
}

template<>
ErrorOr<Audio::SharedAudioFrameRing> decode(Decoder& decoder)
{
    auto buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return Audio::SharedAudioFrameRing::create(move(buffer));
}

}

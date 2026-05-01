/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Vector.h>

#include "sonic/sonic.h"

#include "SonicTimeStretcher.h"

// Set to 1 to enable verbose tracing of push/emit timing.
#define SONIC_STRETCH_TRACE 0

namespace Audio {

struct SonicTimeStretcher::Impl {
    int sample_rate;
    int channel_count;

    sonicStream stream { nullptr };

    float rate { 1.0f };

    i64 position_origin_media_frame { 0 }; // upstream frame at input_consumed_post_seek == 0
    i64 next_output_frame_index { 0 };
    i64 expected_next_input_media_frame { 0 }; // for gap detection in push_block
    i64 input_consumed_post_seek { 0 };

    bool eos_signalled { false };
    bool eos_drained { false };
    Optional<SampleSpecification> latest_input_spec;

    Impl(int sample_rate, int channel_count)
        : sample_rate(sample_rate)
        , channel_count(channel_count)
    {
        stream = sonicCreateStream(sample_rate, channel_count);
        VERIFY(stream != nullptr);
    }

    ~Impl()
    {
        if (stream)
            sonicDestroyStream(stream);
    }

    void recreate_stream()
    {
        if (stream)
            sonicDestroyStream(stream);
        stream = sonicCreateStream(sample_rate, channel_count);
        VERIFY(stream != nullptr);
        sonicSetSpeed(stream, rate);
    }

    // Emit chunk: 30 ms at the operating sample rate (matches signalsmith's
    // intervalSamples for easier A/B comparison of emit cadence).
    int output_chunk_frames() const
    {
        return AK::round_to<int>(sample_rate * 0.03);
    }
};

ErrorOr<NonnullOwnPtr<TimeStretcher>> SonicTimeStretcher::create(u32 sample_rate, u8 channel_count)
{
    if (sample_rate > NumericLimits<int>::max())
        return Error::from_string_literal("Sample rate is too large");
    if (channel_count == 0)
        return Error::from_string_literal("Channel count must be > 0");

    auto impl = adopt_own(*new Impl(static_cast<int>(sample_rate), channel_count));
    return adopt_own(*new SonicTimeStretcher(move(impl)));
}

SonicTimeStretcher::SonicTimeStretcher(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

SonicTimeStretcher::~SonicTimeStretcher() = default;

void SonicTimeStretcher::set_rate(float rate)
{
    VERIFY(rate > 0.0f);
    m_impl->rate = rate;
    sonicSetSpeed(m_impl->stream, rate);
}

void SonicTimeStretcher::set_pitch(float pitch)
{
    VERIFY(pitch > 0.0f);
    sonicSetPitch(m_impl->stream, pitch);
}

i64 SonicTimeStretcher::flush_and_get_preroll(AK::Duration media_start_timestamp, i64 output_start_frame_index)
{
    // Sonic has no in-place reset, but tearing down and recreating the stream is
    // cheap and ensures a clean state.
    m_impl->recreate_stream();
    m_impl->input_consumed_post_seek = 0;
    m_impl->eos_signalled = false;
    m_impl->eos_drained = false;

    auto media_start_in_frames = media_start_timestamp.to_time_units(1, m_impl->sample_rate);
    m_impl->position_origin_media_frame = media_start_in_frames;
    m_impl->next_output_frame_index = output_start_frame_index;
    m_impl->expected_next_input_media_frame = media_start_in_frames;

    // Sonic's PICOLA/overlap-add is mostly local — internal latency is one pitch
    // period (~3-15 ms). We don't ask TSP to seek upstream backwards; the small
    // residual lag is below human perceptibility for transition smoothness.
#if SONIC_STRETCH_TRACE
    dbgln("[sonic] flush: media_start={} (={}fr) output_anchor={} rate={}",
        media_start_timestamp, media_start_in_frames, output_start_frame_index, m_impl->rate);
#endif
    return 0;
}

void SonicTimeStretcher::push_block(Media::AudioBlock const& input)
{
    VERIFY(!input.is_empty());
    VERIFY(input.channel_count() == m_impl->channel_count);
    VERIFY(static_cast<int>(input.sample_rate()) == m_impl->sample_rate);

    // Upstream must be unscaled (no chained stretchers).
    auto expected_start = AK::Duration::from_time_units(input.first_frame_index(), 1, m_impl->sample_rate);
    auto expected_duration = AK::Duration::from_time_units(static_cast<i64>(input.frame_count()), 1, m_impl->sample_rate);
    VERIFY(input.media_time_start() == expected_start);
    VERIFY(input.media_time_duration() == expected_duration);

    m_impl->latest_input_spec = input.sample_specification();

    auto block_start = input.first_frame_index();
    auto frame_count = static_cast<int>(input.frame_count());
    int frames_to_skip = 0;

    auto gap = block_start - m_impl->expected_next_input_media_frame;
    if (gap > 0) {
        // Upstream skipped frames — silence-pad to keep alignment.
        constexpr int max_silence_chunk = 4096;
        Vector<float> silence;
        silence.resize(static_cast<size_t>(max_silence_chunk) * m_impl->channel_count);
        for (auto& sample : silence)
            sample = 0.0f;
        while (gap > 0) {
            int chunk_frame_count = static_cast<int>(min<i64>(gap, max_silence_chunk));
            int ok = sonicWriteFloatToStream(m_impl->stream, silence.data(), chunk_frame_count);
            VERIFY(ok != 0);
            gap -= chunk_frame_count;
        }
    } else if (gap < 0) {
        // Upstream re-delivered overlap — skip the overlapping prefix.
        frames_to_skip = static_cast<int>(min<i64>(-gap, frame_count));
        if (frames_to_skip == frame_count) {
            m_impl->expected_next_input_media_frame = max(
                m_impl->expected_next_input_media_frame,
                block_start + frame_count);
            return;
        }
    }

    auto frames_to_append = frame_count - frames_to_skip;
    auto const* sample_base = input.data().data() + (static_cast<size_t>(frames_to_skip) * m_impl->channel_count);
    int ok = sonicWriteFloatToStream(m_impl->stream, sample_base, frames_to_append);
    VERIFY(ok != 0);
    m_impl->expected_next_input_media_frame = block_start + frame_count;
#if SONIC_STRETCH_TRACE
    dbgln("[sonic] push_block: first_frame_index={} frame_count={} skipped={} silence_padded={}",
        block_start, frame_count, frames_to_skip, gap > 0 ? gap : 0);
#endif
}

void SonicTimeStretcher::signal_end_of_stream()
{
    m_impl->eos_signalled = true;
}

Media::DecoderErrorOr<Media::AudioBlock> SonicTimeStretcher::retrieve_block()
{
    auto& impl = *m_impl;
    int target_output_count = impl.output_chunk_frames();
    int available = sonicSamplesAvailable(impl.stream);

    // If EOS is signalled but we don't yet have a full chunk buffered, force
    // sonic to flush its remaining internal state so we can drain it.
    if (impl.eos_signalled && !impl.eos_drained && available < target_output_count) {
        sonicFlushStream(impl.stream);
        impl.eos_drained = true;
        available = sonicSamplesAvailable(impl.stream);
    }

    bool can_emit_partial = impl.eos_drained;
    if (available == 0 || (available < target_output_count && !can_emit_partial)) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::NeedsMoreInput,
            impl.eos_drained ? "Sonic stream fully drained"sv : "Sonic needs more input"sv);
    }

    if (!impl.latest_input_spec.has_value()) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::Invalid,
            "Time-stretcher emitted output before any input was pushed"sv);
    }

    int read_count = min(available, target_output_count);
    auto media_start_in_frames = impl.position_origin_media_frame + impl.input_consumed_post_seek;
    auto media_duration_in_frames = AK::round_to<i64>(read_count * static_cast<double>(impl.rate));

    Media::AudioBlock output_block;
    output_block.emplace(impl.latest_input_spec.value(), impl.next_output_frame_index, [&](Media::AudioBlock::Data& data) {
        data.resize_and_keep_capacity(static_cast<size_t>(read_count) * impl.channel_count);
        int got = sonicReadFloatFromStream(impl.stream, data.data(), read_count);
        VERIFY(got == read_count);
    });
    output_block.set_media_time_start(AK::Duration::from_time_units(media_start_in_frames, 1, impl.sample_rate));
    output_block.set_media_time_duration(AK::Duration::from_time_units(media_duration_in_frames, 1, impl.sample_rate));

    impl.next_output_frame_index += read_count;
    impl.input_consumed_post_seek += media_duration_in_frames;

#if SONIC_STRETCH_TRACE
    dbgln("[sonic] retrieve: read={} media_start={}fr media_duration={}fr available_after={}",
        read_count, media_start_in_frames, media_duration_in_frames, sonicSamplesAvailable(impl.stream));
#endif

    return output_block;
}

}

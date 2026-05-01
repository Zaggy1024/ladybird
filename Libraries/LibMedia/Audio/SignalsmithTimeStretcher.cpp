/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include "SignalsmithTimeStretcher.h"

// Set to 1 to enable verbose tracing of warmup/emit timing for alignment debugging.
#define SIGNALSMITH_STRETCH_TRACE 0

namespace Audio {

static constexpr int INPUT_BUFFER_MARGIN_FRAMES = 8192;

// Stack adapter satisfying signalsmith's Inputs/Outputs concept (anything
// where view[channel][frame] resolves to a sample). Avoids allocating a
// Vector<float*> per process() call.
struct PlanarView {
    float* base;
    int stride;
    float* operator[](int channel) const { return base + (static_cast<ptrdiff_t>(channel) * stride); }
};

struct SignalsmithTimeStretcher::Impl {
    int sample_rate;
    int channel_count;

    signalsmith::stretch::SignalsmithStretch<float> stretch;

    // Per-channel deinterleaved input; input_planar[channel * stride + frame]
    // is the channel's frame-th valid sample in [0, input_valid_count).
    int stride { 0 };
    Vector<float> input_planar;
    int input_valid_count { 0 };

    // Output scratch, sized to the largest emit chunk per channel.
    Vector<float> output_planar;

    float rate { 1.0f };

    i64 position_origin_media_frame { 0 }; // upstream frame at input_consumed_post_seek == 0
    i64 next_output_frame_index { 0 };
    i64 expected_next_input_media_frame { 0 }; // for gap detection in push_block
    i64 input_consumed_post_seek { 0 };

    // Frames remaining in the warmup process() call that primes the OLA. We
    // feed input via plain process() instead of the library's seek() because
    // seek() yields partial OLA at the first emit (audible crossfade artifact).
    int warmup_input_frames_remaining { 0 };

    bool eos_signalled { false };
    bool eos_drained { false };
    Optional<SampleSpecification> latest_input_spec;

    Impl(int sample_rate, int channel_count)
        : sample_rate(sample_rate)
        , channel_count(channel_count)
    {
        stretch.presetDefault(channel_count, static_cast<float>(sample_rate));
        output_planar.resize(static_cast<size_t>(stretch.intervalSamples()) * static_cast<size_t>(channel_count));
        // input_planar is allocated lazily by the first ensure_input_buffer_capacity()
        // call from set_rate.
    }

    int regular_emit_input_frames() const
    {
        return AK::clamp_to<int>(max<i64>(1, AK::round_to<i64>(stretch.intervalSamples() * static_cast<double>(rate))));
    }

    // Warmup feeds enough input that the first regular emit has full OLA (4
    // grains) of all-real content: T ≥ windowSize/rate + 3×interval, rounded
    // up to an interval multiple; warmup_input = T × rate.
    int total_warmup_input_frames() const
    {
        auto interval_samples = stretch.intervalSamples();
        auto rate_as_double = static_cast<double>(rate);
        auto min_synthesis_position = (stretch.blockSamples() / rate_as_double) + (3 * interval_samples);
        return AK::round_to<int>(AK::ceil(min_synthesis_position / interval_samples) * interval_samples * rate_as_double);
    }

    // Grow input_planar so a single process() call can hold the largest input
    // we'll pass it. Without this, high rates cap input_valid_count below the
    // threshold retrieve_block waits for, stalling in a drop_input_front loop.
    void ensure_input_buffer_capacity()
    {
        int required = max(max(total_warmup_input_frames(), regular_emit_input_frames()), warmup_input_frames_remaining);
        int required_stride = required + INPUT_BUFFER_MARGIN_FRAMES;
        if (required_stride <= stride)
            return;

        Vector<float> new_buffer;
        new_buffer.resize(static_cast<size_t>(required_stride) * static_cast<size_t>(channel_count));
        for (int channel = 0; channel < channel_count; ++channel) {
            for (int frame = 0; frame < input_valid_count; ++frame)
                new_buffer[(static_cast<size_t>(channel) * required_stride) + frame] = input_planar[(static_cast<size_t>(channel) * stride) + frame];
        }
        input_planar = move(new_buffer);
        stride = required_stride;
    }

    void make_room_for_input_frames(int frame_count)
    {
        VERIFY(frame_count >= 0);
        if (input_valid_count + frame_count > stride)
            drop_input_front((input_valid_count + frame_count) - stride);
    }

    void drop_input_front(int frame_count)
    {
        VERIFY(frame_count >= 0);
        VERIFY(frame_count <= input_valid_count);
        if (frame_count == 0)
            return;
        for (int channel = 0; channel < channel_count; ++channel) {
            auto* channel_base = input_planar.data() + (static_cast<ptrdiff_t>(channel) * stride);
            for (int frame = 0; frame < input_valid_count - frame_count; ++frame)
                channel_base[frame] = channel_base[frame + frame_count];
        }
        input_valid_count -= frame_count;
    }

    void append_silence_to_input(int frame_count)
    {
        make_room_for_input_frames(frame_count);
        for (int channel = 0; channel < channel_count; ++channel) {
            auto* channel_base = input_planar.data() + (static_cast<ptrdiff_t>(channel) * stride);
            for (int frame = 0; frame < frame_count; ++frame)
                channel_base[input_valid_count + frame] = 0.0f;
        }
        input_valid_count += frame_count;
    }

    void append_interleaved_to_input(float const* interleaved, int frame_offset, int frame_count)
    {
        make_room_for_input_frames(frame_count);
        for (int channel = 0; channel < channel_count; ++channel) {
            auto* channel_base = input_planar.data() + (static_cast<ptrdiff_t>(channel) * stride);
            for (int frame = 0; frame < frame_count; ++frame)
                channel_base[input_valid_count + frame] = interleaved[((frame_offset + frame) * channel_count) + channel];
        }
        input_valid_count += frame_count;
    }

    PlanarView input_view() { return { input_planar.data(), stride }; }

    // Build an AudioBlock from output_planar's first frame_count frames per
    // channel, advance next_output_frame_index. Caller must have ensured
    // latest_input_spec is set.
    Media::AudioBlock build_output_block(int frame_count, i64 media_start_in_frames, i64 media_duration_in_frames)
    {
        Media::AudioBlock output_block;
        output_block.emplace(latest_input_spec.value(), next_output_frame_index, [&](Media::AudioBlock::Data& data) {
            data.resize_and_keep_capacity(static_cast<size_t>(frame_count) * channel_count);
            for (int frame = 0; frame < frame_count; ++frame) {
                for (int channel = 0; channel < channel_count; ++channel)
                    data[(frame * channel_count) + channel] = output_planar[(static_cast<ptrdiff_t>(channel) * frame_count) + frame];
            }
        });
        output_block.set_media_time_start(AK::Duration::from_time_units(media_start_in_frames, 1, sample_rate));
        output_block.set_media_time_duration(AK::Duration::from_time_units(media_duration_in_frames, 1, sample_rate));
        next_output_frame_index += frame_count;
        return output_block;
    }
};

ErrorOr<NonnullOwnPtr<TimeStretcher>> SignalsmithTimeStretcher::create(u32 sample_rate, u8 channel_count)
{
    if (sample_rate > NumericLimits<int>::max())
        return Error::from_string_literal("Sample rate is too large");
    if (channel_count == 0)
        return Error::from_string_literal("Channel count must be > 0");

    auto impl = adopt_own(*new Impl(static_cast<int>(sample_rate), channel_count));
    return adopt_own(*new SignalsmithTimeStretcher(move(impl)));
}

SignalsmithTimeStretcher::SignalsmithTimeStretcher(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

SignalsmithTimeStretcher::~SignalsmithTimeStretcher() = default;

void SignalsmithTimeStretcher::set_rate(float rate)
{
    VERIFY(rate > 0.0f);
    m_impl->rate = rate;
    m_impl->ensure_input_buffer_capacity();
}

void SignalsmithTimeStretcher::set_pitch(float pitch)
{
    VERIFY(pitch > 0.0f);
    // tonalityLimit=0 disables formant preservation; for pure time-stretch the
    // multiplier stays at 1 anyway, so this is a no-op in our typical pipeline.
    m_impl->stretch.setTransposeFactor(pitch);
}

i64 SignalsmithTimeStretcher::flush_and_get_preroll(AK::Duration media_start_timestamp, i64 output_start_frame_index)
{
    m_impl->stretch.reset();
    m_impl->input_valid_count = 0;
    m_impl->input_consumed_post_seek = 0;
    m_impl->eos_signalled = false;
    m_impl->eos_drained = false;

    auto warmup_input_frame_count = m_impl->total_warmup_input_frames();
    m_impl->warmup_input_frames_remaining = warmup_input_frame_count;

    auto media_start_in_frames = media_start_timestamp.to_time_units(1, m_impl->sample_rate);
    m_impl->position_origin_media_frame = media_start_in_frames;
    m_impl->next_output_frame_index = output_start_frame_index;

    // Library's total lag is blockSamples; after N inputs the next output
    // reflects content at (N - blockSamples). To land the first emit at
    // media_target, ask TSP to seek (warmup_input - blockSamples) before it.
    auto preroll_returned = static_cast<i64>(warmup_input_frame_count - m_impl->stretch.blockSamples());
    VERIFY(preroll_returned > 0);
    m_impl->expected_next_input_media_frame = media_start_in_frames - preroll_returned;
#if SIGNALSMITH_STRETCH_TRACE
    dbgln("[signalsmith] flush: media_start={} (={}fr) output_anchor={} rate={} preroll_returned={}fr warmup_input={}fr blockSamples={} intervalSamples={} inputLatency={} outputLatency={}",
        media_start_timestamp, media_start_in_frames, output_start_frame_index, m_impl->rate,
        preroll_returned, warmup_input_frame_count, m_impl->stretch.blockSamples(), m_impl->stretch.intervalSamples(),
        m_impl->stretch.inputLatency(), m_impl->stretch.outputLatency());
#endif
    return preroll_returned;
}

void SignalsmithTimeStretcher::push_block(Media::AudioBlock const& input)
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
        while (gap > 0) {
            int chunk_frame_count = static_cast<int>(min<i64>(gap, m_impl->stride));
            m_impl->append_silence_to_input(chunk_frame_count);
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
    m_impl->append_interleaved_to_input(input.data().data(), frames_to_skip, frames_to_append);
    m_impl->expected_next_input_media_frame = block_start + frame_count;
#if SIGNALSMITH_STRETCH_TRACE
    dbgln("[signalsmith] push_block: first_frame_index={} frame_count={} skipped={} silence_padded={} input_valid_count={}",
        block_start, frame_count, frames_to_skip, gap > 0 ? gap : 0, m_impl->input_valid_count);
#endif
}

void SignalsmithTimeStretcher::signal_end_of_stream()
{
    m_impl->eos_signalled = true;
}

Media::DecoderErrorOr<Media::AudioBlock> SignalsmithTimeStretcher::retrieve_block()
{
    auto channel_count = m_impl->channel_count;

    // Phase 1: warmup. Discard output; primes the OLA so the first regular
    // emit lands at media_target with full 4-grain reconstruction.
    if (m_impl->warmup_input_frames_remaining > 0) {
        int warmup_input_count = m_impl->warmup_input_frames_remaining;
        int warmup_output_count = AK::clamp_to<int>(
            max<i64>(1, AK::round_to<i64>(warmup_input_count / static_cast<double>(m_impl->rate))));

        // ensure_input_buffer_capacity() must have sized the buffer to fit this
        // threshold; otherwise input_valid_count saturates below it and we'd loop.
        VERIFY(warmup_input_count <= m_impl->stride);

        if (m_impl->input_valid_count < warmup_input_count) {
#if SIGNALSMITH_STRETCH_TRACE
            dbgln("[signalsmith] retrieve: warmup waiting (have={} need={})",
                m_impl->input_valid_count, warmup_input_count);
#endif
            return Media::DecoderError::with_description(
                Media::DecoderErrorCategory::NeedsMoreInput,
                "Time-stretcher needs more input for warmup"sv);
        }

        // Resize output scratch on demand (slow rates can need several windowSizes).
        size_t required_planar_size = static_cast<size_t>(warmup_output_count) * channel_count;
        if (m_impl->output_planar.size() < required_planar_size)
            m_impl->output_planar.resize(required_planar_size);

        PlanarView output_view { m_impl->output_planar.data(), warmup_output_count };
#if SIGNALSMITH_STRETCH_TRACE
        dbgln("[signalsmith] retrieve: warmup process input={} output={} (rate={})",
            warmup_input_count, warmup_output_count, m_impl->rate);
#endif
        m_impl->stretch.process(m_impl->input_view(), warmup_input_count, output_view, warmup_output_count);
        m_impl->drop_input_front(warmup_input_count);
        m_impl->warmup_input_frames_remaining = 0;
    }

    // Phase 2: regular process().
    int output_count = m_impl->stretch.intervalSamples();
    int input_count = m_impl->regular_emit_input_frames();

    VERIFY(input_count <= m_impl->stride);

    if (m_impl->input_valid_count < input_count) {
        if (m_impl->eos_signalled && !m_impl->eos_drained) {
            // Drain via flush(); subsequent calls return NeedsMoreInput permanently.
            m_impl->eos_drained = true;
            int drain_frames = m_impl->stretch.outputLatency();
            if (!m_impl->latest_input_spec.has_value()) {
                return Media::DecoderError::with_description(
                    Media::DecoderErrorCategory::Invalid,
                    "Time-stretcher emitted output before any input was pushed"sv);
            }
            if (static_cast<int>(m_impl->output_planar.size()) < drain_frames * channel_count)
                m_impl->output_planar.resize(static_cast<size_t>(drain_frames) * channel_count);
            PlanarView output_view { m_impl->output_planar.data(), drain_frames };
            m_impl->stretch.flush(output_view, drain_frames);

            auto media_start_in_frames = m_impl->position_origin_media_frame + m_impl->input_consumed_post_seek;
            // Drain has no input; estimate duration as drain_frames × rate.
            auto media_duration_in_frames = AK::round_to<i64>(drain_frames * static_cast<double>(m_impl->rate));
            m_impl->input_consumed_post_seek += media_duration_in_frames;
            return m_impl->build_output_block(drain_frames, media_start_in_frames, media_duration_in_frames);
        }

        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::NeedsMoreInput,
            "Time-stretcher needs more input to produce output"sv);
    }

    if (!m_impl->latest_input_spec.has_value()) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::Invalid,
            "Time-stretcher emitted output before any input was pushed"sv);
    }

    PlanarView output_view { m_impl->output_planar.data(), output_count };
    m_impl->stretch.process(m_impl->input_view(), input_count, output_view, output_count);

    auto media_start_in_frames = m_impl->position_origin_media_frame + m_impl->input_consumed_post_seek;
    auto media_duration_in_frames = static_cast<i64>(input_count);

#if SIGNALSMITH_STRETCH_TRACE
    bool first_emit = (m_impl->input_consumed_post_seek == 0);
    if (first_emit) {
        dbgln("[signalsmith] retrieve: FIRST EMIT input={} output={} first_frame_index={} media_time_start={}fr (={}us) media_time_duration={}fr position_origin={}fr input_consumed_pre={}",
            input_count, output_count, m_impl->next_output_frame_index,
            media_start_in_frames, AK::Duration::from_time_units(media_start_in_frames, 1, m_impl->sample_rate).to_microseconds(),
            media_duration_in_frames, m_impl->position_origin_media_frame, m_impl->input_consumed_post_seek);
    }
#endif

    m_impl->drop_input_front(input_count);
    m_impl->input_consumed_post_seek += input_count;
    return m_impl->build_output_block(output_count, media_start_in_frames, media_duration_in_frames);
}

}

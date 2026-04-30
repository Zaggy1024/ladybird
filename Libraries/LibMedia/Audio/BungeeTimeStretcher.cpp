/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Bungee's headers use std::vector / std::round / std::isnan without pulling in the
// corresponding standard headers — include them here before Bungee.
#include <cmath>
#include <vector>

#include <AK/Math.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <bungee/Bungee.h>
#include <bungee/Stream.h>

#include "BungeeTimeStretcher.h"

namespace Audio {

static constexpr int LOG2_SYNTHESIS_HOP_ADJUST = 0;
static constexpr int STREAM_MAX_INPUT_FRAMES_PER_PROCESS = 32768;

struct BungeeTimeStretcher::Impl {
    int sample_rate;
    int channel_count;

    Bungee::Stretcher<Bungee::Basic> stretcher;
    Bungee::Stream<Bungee::Basic> stream;

    // Interleaved pending input awaiting processing.
    Vector<float> pending_interleaved;
    int pending_frame_count { 0 };

    // Per-channel scratch buffers (rebuilt per retrieve_block call as needed).
    Vector<Vector<float>> planar_input;
    Vector<Vector<float>> planar_output;
    Vector<float const*> input_ptrs;
    Vector<float*> output_ptrs;

    float rate { 1.0f };
    float pitch { 1.0f };

    i64 position_origin_media_frame { 0 };
    i64 next_output_media_frame { 0 };
    i64 expected_next_input_bungee_position { 0 };
    bool eos_signalled { false };
    bool needs_eos_drain { false };
    Optional<SampleSpecification> latest_input_spec;

    Impl(int sr, int cc)
        : sample_rate(sr)
        , channel_count(cc)
        , stretcher(Bungee::SampleRates { sr, sr }, cc, LOG2_SYNTHESIS_HOP_ADJUST)
        , stream(stretcher, STREAM_MAX_INPUT_FRAMES_PER_PROCESS, cc)
    {
        planar_input.resize(cc);
        planar_output.resize(cc);
        input_ptrs.resize(cc);
        output_ptrs.resize(cc);
    }

    void recreate_processing_state()
    {
        // The Bungee Stretcher accumulates analyzer state across grains. Reusing a
        // used Stretcher with a freshly constructed Stream causes specifyGrain to
        // assert on subsequent calls, so rebuild both. (Stream holds a reference to
        // Stretcher, so order matters: tear down Stream first, then Stretcher.)
        stream.~Stream();
        stretcher.~Stretcher();
        new (&stretcher) Bungee::Stretcher<Bungee::Basic>(
            Bungee::SampleRates { sample_rate, sample_rate },
            channel_count,
            LOG2_SYNTHESIS_HOP_ADJUST);
        new (&stream) Bungee::Stream<Bungee::Basic>(
            stretcher, STREAM_MAX_INPUT_FRAMES_PER_PROCESS, channel_count);
    }

    void append_silence(int frame_count)
    {
        if (frame_count <= 0)
            return;
        auto base = pending_interleaved.size();
        pending_interleaved.resize(base + (static_cast<size_t>(frame_count) * channel_count));
        for (size_t i = base; i < pending_interleaved.size(); ++i)
            pending_interleaved[i] = 0.0f;
        pending_frame_count += frame_count;
    }

    void append_interleaved(float const* src, int frame_offset, int frame_count)
    {
        if (frame_count <= 0)
            return;
        auto base = pending_interleaved.size();
        pending_interleaved.resize(base + (static_cast<size_t>(frame_count) * channel_count));
        for (int i = 0; i < frame_count; ++i) {
            for (int c = 0; c < channel_count; ++c)
                pending_interleaved[base + (i * channel_count) + c]
                    = src[((frame_offset + i) * channel_count) + c];
        }
        pending_frame_count += frame_count;
    }
};

NonnullOwnPtr<TimeStretcher> BungeeTimeStretcher::create(u32 sample_rate, u8 channel_count)
{
    auto impl = adopt_own(*new Impl(static_cast<int>(sample_rate), static_cast<int>(channel_count)));
    return adopt_own(*new BungeeTimeStretcher(move(impl)));
}

BungeeTimeStretcher::BungeeTimeStretcher(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

BungeeTimeStretcher::~BungeeTimeStretcher() = default;

void BungeeTimeStretcher::set_rate(float rate)
{
    VERIFY(rate > 0.0f);
    m_impl->rate = rate;
}

void BungeeTimeStretcher::set_pitch(float pitch)
{
    VERIFY(pitch > 0.0f);
    m_impl->pitch = pitch;
}

size_t BungeeTimeStretcher::preroll_input_frames() const
{
    // Preroll is not currently implemented; Bungee::Stream's mute-head handling at the
    // start of stream produces slightly weak first ms but correct timing.
    return 0;
}

void BungeeTimeStretcher::flush(AK::Duration target_start_timestamp)
{
    auto target_in_frames = target_start_timestamp.to_time_units(1, m_impl->sample_rate);
    m_impl->position_origin_media_frame = target_in_frames;
    m_impl->next_output_media_frame = target_in_frames;
    m_impl->expected_next_input_bungee_position = 0;
    m_impl->pending_interleaved.clear();
    m_impl->pending_frame_count = 0;
    m_impl->eos_signalled = false;
    m_impl->needs_eos_drain = false;
    m_impl->recreate_processing_state();
}

void BungeeTimeStretcher::push_block(Media::AudioBlock const& input)
{
    VERIFY(!input.is_empty());
    VERIFY(input.channel_count() == m_impl->channel_count);
    VERIFY(static_cast<int>(input.sample_rate()) == m_impl->sample_rate);

    m_impl->latest_input_spec = input.sample_specification();

    auto block_bungee_start = input.timestamp_in_frames() - m_impl->position_origin_media_frame;
    auto frame_count = static_cast<int>(input.frame_count());
    int frames_to_skip = 0;

    auto gap = block_bungee_start - m_impl->expected_next_input_bungee_position;
    if (gap > 0) {
        m_impl->append_silence(static_cast<int>(min<i64>(gap, NumericLimits<int>::max())));
    } else if (gap < 0) {
        frames_to_skip = static_cast<int>(min<i64>(-gap, frame_count));
        if (frames_to_skip == frame_count) {
            m_impl->expected_next_input_bungee_position = max(
                m_impl->expected_next_input_bungee_position,
                block_bungee_start + frame_count);
            return;
        }
    }

    auto frames_to_keep = frame_count - frames_to_skip;
    m_impl->append_interleaved(input.data().data(), frames_to_skip, frames_to_keep);
    m_impl->expected_next_input_bungee_position = block_bungee_start + frame_count;
}

void BungeeTimeStretcher::signal_end_of_stream()
{
    m_impl->eos_signalled = true;
    m_impl->needs_eos_drain = true;
}

Media::DecoderErrorOr<Media::AudioBlock> BungeeTimeStretcher::retrieve_block()
{
    auto channel_count = static_cast<size_t>(m_impl->channel_count);

    // We need either pushed input or an EOS-drain situation.
    if (m_impl->pending_frame_count == 0) {
        if (!m_impl->needs_eos_drain) {
            return Media::DecoderError::with_description(
                Media::DecoderErrorCategory::NeedsMoreInput,
                "Time-stretcher needs more input to produce output"sv);
        }
        // Nothing left to feed; mark fully drained.
        m_impl->needs_eos_drain = false;
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::NeedsMoreInput,
            "Time-stretcher fully drained at end of stream"sv);
    }

    int input_frame_count = min(m_impl->pending_frame_count, STREAM_MAX_INPUT_FRAMES_PER_PROCESS);

    // Deinterleave the input chunk into per-channel scratch.
    for (size_t c = 0; c < channel_count; ++c) {
        m_impl->planar_input[c].resize(input_frame_count);
        for (int i = 0; i < input_frame_count; ++i)
            m_impl->planar_input[c][i] = m_impl->pending_interleaved[(i * channel_count) + c];
        m_impl->input_ptrs[c] = m_impl->planar_input[c].data();
    }

    auto rate = m_impl->rate;
    double output_frame_count_ideal = static_cast<double>(input_frame_count) / static_cast<double>(rate);
    int output_capacity = static_cast<int>(std::ceil(output_frame_count_ideal)) + 2;

    for (size_t c = 0; c < channel_count; ++c) {
        m_impl->planar_output[c].resize(output_capacity);
        m_impl->output_ptrs[c] = m_impl->planar_output[c].data();
    }

    int output_frames = m_impl->stream.process(
        m_impl->input_ptrs.data(),
        m_impl->output_ptrs.data(),
        input_frame_count,
        output_frame_count_ideal,
        static_cast<double>(m_impl->pitch));

    // Discard the consumed input frames from pending.
    auto consumed_samples = static_cast<size_t>(input_frame_count) * channel_count;
    if (consumed_samples == m_impl->pending_interleaved.size()) {
        m_impl->pending_interleaved.clear();
    } else {
        for (size_t i = consumed_samples; i < m_impl->pending_interleaved.size(); ++i)
            m_impl->pending_interleaved[i - consumed_samples] = m_impl->pending_interleaved[i];
        m_impl->pending_interleaved.resize(m_impl->pending_interleaved.size() - consumed_samples);
    }
    m_impl->pending_frame_count -= input_frame_count;

    if (output_frames <= 0) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::NeedsMoreInput,
            "Time-stretcher produced no output for this input chunk"sv);
    }

    if (!m_impl->latest_input_spec.has_value()) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::Invalid,
            "Time-stretcher emitted output before any input was pushed"sv);
    }

    auto out_anchor_in_frames = m_impl->next_output_media_frame;
    Media::AudioBlock out;
    out.emplace(m_impl->latest_input_spec.value(), out_anchor_in_frames, [&](Media::AudioBlock::Data& data) {
        data.resize_and_keep_capacity(static_cast<size_t>(output_frames) * channel_count);
        for (int i = 0; i < output_frames; ++i) {
            for (size_t c = 0; c < channel_count; ++c)
                data[(i * channel_count) + c] = m_impl->planar_output[c][i];
        }
    });
    auto input_frame_count_for_output = static_cast<i64>(static_cast<double>(output_frames) * static_cast<double>(rate));
    out.set_input_frame_count(input_frame_count_for_output);
    m_impl->next_output_media_frame += output_frames;
    return out;
}

}

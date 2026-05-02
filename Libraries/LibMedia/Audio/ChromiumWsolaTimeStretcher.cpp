/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>

#include "Chromium/AudioBus.h"
#include "Chromium/AudioRendererAlgorithm.h"

#include "ChromiumWsolaTimeStretcher.h"

// Set to 1 to enable verbose tracing of warmup/emit timing.
#define CHROMIUM_WSOLA_TRACE 0

namespace Audio {

struct ChromiumWsolaTimeStretcher::Impl {
    int sample_rate;
    int channel_count;

    OwnPtr<Chromium::AudioRendererAlgorithm> algorithm;

    float rate { 1.0f };

    i64 position_origin_media_frame { 0 }; // upstream frame at input_consumed_post_seek == 0
    i64 next_output_frame_index { 0 };
    i64 expected_next_input_media_frame { 0 }; // for gap detection in push_block
    i64 input_consumed_post_seek { 0 };

    bool eos_signalled { false };
    bool eos_drained { false };
    Optional<SampleSpecification> latest_input_spec;

    // Output emit chunk: 30 ms — matches signalsmith/sonic for A/B comparison.
    int output_chunk_frames() const
    {
        return AK::round_to<int>(sample_rate * 0.03);
    }

    Impl(int sample_rate, int channel_count)
        : sample_rate(sample_rate)
        , channel_count(channel_count)
    {
        algorithm = make<Chromium::AudioRendererAlgorithm>(sample_rate, channel_count);
    }
};

ErrorOr<NonnullOwnPtr<TimeStretcher>> ChromiumWsolaTimeStretcher::create(u32 sample_rate, u8 channel_count)
{
    if (sample_rate > NumericLimits<int>::max())
        return Error::from_string_literal("Sample rate is too large");
    if (channel_count == 0)
        return Error::from_string_literal("Channel count must be > 0");

    auto impl = adopt_own(*new Impl(static_cast<int>(sample_rate), channel_count));
    return adopt_own(*new ChromiumWsolaTimeStretcher(move(impl)));
}

ChromiumWsolaTimeStretcher::ChromiumWsolaTimeStretcher(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

ChromiumWsolaTimeStretcher::~ChromiumWsolaTimeStretcher() = default;

void ChromiumWsolaTimeStretcher::set_rate(float rate)
{
    VERIFY(rate > 0.0f);
    m_impl->rate = rate;
    // The algorithm reads playback_rate per fill_buffer call; nothing to set here.
}

void ChromiumWsolaTimeStretcher::set_pitch(float)
{
    // Chromium's WSOLA preserves pitch by construction. Pitch shifting requires
    // resampling on top, which we don't implement in this experiment.
}

i64 ChromiumWsolaTimeStretcher::flush_and_get_preroll(AK::Duration media_start_timestamp, i64 output_start_frame_index)
{
    m_impl->algorithm->flush_buffers();
    m_impl->input_consumed_post_seek = 0;
    m_impl->eos_signalled = false;
    m_impl->eos_drained = false;

    auto media_start_in_frames = media_start_timestamp.to_time_units(1, m_impl->sample_rate);
    m_impl->position_origin_media_frame = media_start_in_frames;
    m_impl->next_output_frame_index = output_start_frame_index;
    m_impl->expected_next_input_media_frame = media_start_in_frames;

    // Chromium's WSOLA produces a half-window fade-in at the start of fresh
    // output (the first ola_hop_size frames are amplitude-windowed). Returning
    // 0 preroll keeps timing simple; the fade-in is brief (~10 ms) and only
    // perceptible at hard transitions.
#if CHROMIUM_WSOLA_TRACE
    dbgln("[chromium-wsola] flush: media_start={} (={}fr) output_anchor={} rate={}",
        media_start_timestamp, media_start_in_frames, output_start_frame_index, m_impl->rate);
#endif
    return 0;
}

void ChromiumWsolaTimeStretcher::push_block(Media::AudioBlock const& input)
{
    VERIFY(!input.is_empty());
    VERIFY(input.channel_count() == m_impl->channel_count);
    VERIFY(static_cast<int>(input.sample_rate()) == m_impl->sample_rate);

    // Upstream must be unscaled.
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
        constexpr int max_silence_chunk = 4096;
        while (gap > 0) {
            int chunk_frame_count = static_cast<int>(min<i64>(gap, max_silence_chunk));
            // Build a silent AudioBus for this chunk and enqueue.
            auto silence = Chromium::AudioBus::create(m_impl->channel_count, chunk_frame_count);
            silence->zero();
            m_impl->algorithm->enqueue_buffer(*silence);
            gap -= chunk_frame_count;
        }
    } else if (gap < 0) {
        frames_to_skip = static_cast<int>(min<i64>(-gap, frame_count));
        if (frames_to_skip == frame_count) {
            m_impl->expected_next_input_media_frame = max(
                m_impl->expected_next_input_media_frame,
                block_start + frame_count);
            return;
        }
    }

    // Convert interleaved input to planar AudioBus, skipping the overlap prefix if needed.
    int const frames_to_append = frame_count - frames_to_skip;
    auto planar_buffer = Chromium::AudioBus::create(m_impl->channel_count, frames_to_append);
    auto const* interleaved = input.data().data();
    for (int channel_index = 0; channel_index < m_impl->channel_count; ++channel_index) {
        auto channel_data = planar_buffer->channel(channel_index);
        for (int frame = 0; frame < frames_to_append; ++frame)
            channel_data[frame] = interleaved[((frames_to_skip + frame) * m_impl->channel_count) + channel_index];
    }

    m_impl->algorithm->enqueue_buffer(*planar_buffer);
    m_impl->expected_next_input_media_frame = block_start + frame_count;
#if CHROMIUM_WSOLA_TRACE
    dbgln("[chromium-wsola] push_block: first_frame_index={} frame_count={} skipped={} silence_padded={} buffered={}",
        block_start, frame_count, frames_to_skip, gap > 0 ? gap : 0, m_impl->algorithm->buffered_frames());
#endif
}

void ChromiumWsolaTimeStretcher::signal_end_of_stream()
{
    m_impl->eos_signalled = true;
    m_impl->algorithm->mark_end_of_stream();
}

Media::DecoderErrorOr<Media::AudioBlock> ChromiumWsolaTimeStretcher::retrieve_block()
{
    auto& impl = *m_impl;
    int const target_output_count = impl.output_chunk_frames();

    // Try to render `target_output_count` frames into a temporary planar bus.
    auto output_planar = Chromium::AudioBus::create(impl.channel_count, target_output_count);
    int const rendered = impl.algorithm->fill_buffer(*output_planar, 0, target_output_count, impl.rate);

    if (rendered == 0) {
        if (impl.eos_signalled && !impl.eos_drained) {
            // Nothing more to render after EOS. Mark drained so future calls are
            // immediate NeedsMoreInput.
            impl.eos_drained = true;
        }
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::NeedsMoreInput,
            impl.eos_drained ? "Chromium WSOLA fully drained"sv : "Chromium WSOLA needs more input"sv);
    }

    if (!impl.latest_input_spec.has_value()) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::Invalid,
            "Time-stretcher emitted output before any input was pushed"sv);
    }

    auto media_start_in_frames = impl.position_origin_media_frame + impl.input_consumed_post_seek;
    auto media_duration_in_frames = AK::round_to<i64>(rendered * static_cast<double>(impl.rate));

    Media::AudioBlock output_block;
    output_block.emplace(impl.latest_input_spec.value(), impl.next_output_frame_index, [&](Media::AudioBlock::Data& data) {
        data.resize_and_keep_capacity(static_cast<size_t>(rendered) * impl.channel_count);
        // Interleave from the planar AudioBus.
        for (int frame = 0; frame < rendered; ++frame) {
            for (int channel_index = 0; channel_index < impl.channel_count; ++channel_index)
                data[(frame * impl.channel_count) + channel_index] = output_planar->channel(channel_index)[frame];
        }
    });
    output_block.set_media_time_start(AK::Duration::from_time_units(media_start_in_frames, 1, impl.sample_rate));
    output_block.set_media_time_duration(AK::Duration::from_time_units(media_duration_in_frames, 1, impl.sample_rate));

    impl.next_output_frame_index += rendered;
    impl.input_consumed_post_seek += media_duration_in_frames;

#if CHROMIUM_WSOLA_TRACE
    dbgln("[chromium-wsola] retrieve: rendered={} media_start={}fr media_duration={}fr buffered={}",
        rendered, media_start_in_frames, media_duration_in_frames, impl.algorithm->buffered_frames());
#endif

    return output_block;
}

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// signalsmith-stretch's headers transitively include the standard <vector>,
// <algorithm>, <functional>, <random> via signalsmith-dsp; pull <cmath> in
// alongside for std::round/std::lround used here.
#include <cmath>

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/TypedTransfer.h>
#include <AK/Vector.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include "SignalsmithTimeStretcher.h"

namespace Audio {

static constexpr int INPUT_BUFFER_MARGIN_FRAMES = 8192;

struct SignalsmithTimeStretcher::Impl {
    int sample_rate;
    int channel_count;

    signalsmith::stretch::SignalsmithStretch<float> stretch;

    // Per-channel deinterleaved input. Layout: input_planar[c * stride + i] is
    // channel c's i-th valid frame in [0, input_valid_count). Sized to hold one
    // block + interval + a generous margin so a single retrieve_block can always
    // emit at least one chunk worth.
    int stride;
    Vector<float> input_planar;
    int input_valid_count { 0 };

    // Output scratch, one chunk's worth of frames per channel.
    Vector<float> output_planar;

    // Knobs (set by TimeStretchProcessor before calls).
    float rate { 1.0f };
    float pitch { 1.0f };

    // After flush_and_get_preroll, position_origin_media_frame is the media-frame
    // index where input_consumed_since_flush == 0 — i.e. where the next emitted
    // block's media_time_start sits at zero offset.
    i64 position_origin_media_frame { 0 };
    i64 next_output_frame_index { 0 };
    // Count of input frames pushed so far this stream, in upstream-frame coordinates.
    // Used to detect/silence-fill gaps from upstream.
    i64 expected_next_input_media_frame { 0 };
    // Input frames consumed by the stretcher (excluding preroll). Drives per-block
    // media_time_start.
    i64 input_consumed_post_seek { 0 };

    // Preroll/seek bookkeeping. seek() must be called once per flush, with at least
    // preroll_target frames of historical input. preroll_pending is true between
    // flush and the seek() call.
    int preroll_target { 0 };
    bool preroll_pending { true };

    bool eos_signalled { false };
    bool eos_drained { false };
    Optional<SampleSpecification> latest_input_spec;

    Impl(int sr, int cc)
        : sample_rate(sr)
        , channel_count(cc)
    {
        stretch.presetDefault(channel_count, static_cast<float>(sample_rate));
        stride = stretch.blockSamples() + stretch.intervalSamples() + INPUT_BUFFER_MARGIN_FRAMES;
        input_planar.resize(static_cast<size_t>(stride) * static_cast<size_t>(channel_count));
        // Output scratch: one chunk × channels.
        output_planar.resize(static_cast<size_t>(stretch.intervalSamples()) * static_cast<size_t>(channel_count));
        preroll_target = stretch.blockSamples() + stretch.intervalSamples();
    }

    int output_chunk_frames() const { return stretch.intervalSamples(); }

    // Number of input frames the stretcher needs to produce `output_count` output
    // frames, given the current rate.
    int input_frames_for_output(int output_count) const
    {
        // ratio = output / input; for our rate convention (>1 = faster playback),
        // input = output * rate.
        auto count = static_cast<i64>(std::lround(static_cast<double>(output_count) * static_cast<double>(rate)));
        return AK::clamp_to<int>(max<i64>(1, count));
    }

    void drop_input_front(int frame_count)
    {
        VERIFY(frame_count >= 0);
        VERIFY(frame_count <= input_valid_count);
        if (frame_count == 0)
            return;
        for (int c = 0; c < channel_count; ++c) {
            auto* base = input_planar.data() + (static_cast<ptrdiff_t>(c) * stride);
            for (int i = 0; i < input_valid_count - frame_count; ++i)
                base[i] = base[i + frame_count];
        }
        input_valid_count -= frame_count;
    }

    void append_silence_to_input(int frame_count)
    {
        VERIFY(frame_count >= 0);
        if (input_valid_count + frame_count > stride) {
            int over = (input_valid_count + frame_count) - stride;
            drop_input_front(over);
        }
        for (int c = 0; c < channel_count; ++c) {
            auto* base = input_planar.data() + (static_cast<ptrdiff_t>(c) * stride);
            for (int i = 0; i < frame_count; ++i)
                base[input_valid_count + i] = 0.0f;
        }
        input_valid_count += frame_count;
    }

    void append_interleaved_to_input(float const* interleaved, int frame_offset, int frame_count)
    {
        VERIFY(frame_count >= 0);
        if (input_valid_count + frame_count > stride) {
            int over = (input_valid_count + frame_count) - stride;
            drop_input_front(over);
        }
        for (int c = 0; c < channel_count; ++c) {
            auto* base = input_planar.data() + (static_cast<ptrdiff_t>(c) * stride);
            for (int i = 0; i < frame_count; ++i)
                base[input_valid_count + i] = interleaved[((frame_offset + i) * channel_count) + c];
        }
        input_valid_count += frame_count;
    }
};

// Helper: build an array of per-channel pointers into a planar buffer. The
// signalsmith API takes Inputs/Outputs as anything where `inputs[c][i]` resolves
// to a sample; a Vector<float*> works fine.
static Vector<float*> channel_pointers_in(Vector<float>& planar, int stride, int channel_count, int frame_offset = 0)
{
    Vector<float*> ptrs;
    ptrs.ensure_capacity(channel_count);
    for (int c = 0; c < channel_count; ++c)
        ptrs.append(planar.data() + (static_cast<ptrdiff_t>(c) * stride) + frame_offset);
    return ptrs;
}

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
}

void SignalsmithTimeStretcher::set_pitch(float pitch)
{
    VERIFY(pitch > 0.0f);
    m_impl->pitch = pitch;
    // tonalityLimit=0 disables formant preservation; for pure time-stretch the
    // multiplier stays at 1 anyway, so this is a no-op in our typical pipeline.
    m_impl->stretch.setTransposeFactor(pitch);
}

i64 SignalsmithTimeStretcher::flush_and_get_preroll(AK::Duration media_start_timestamp, i64 output_start_frame_index)
{
    // Drop any buffered state.
    m_impl->stretch.reset();
    m_impl->input_valid_count = 0;
    m_impl->input_consumed_post_seek = 0;
    m_impl->eos_signalled = false;
    m_impl->eos_drained = false;
    m_impl->preroll_pending = true;

    auto media_start_in_frames = media_start_timestamp.to_time_units(1, m_impl->sample_rate);
    m_impl->position_origin_media_frame = media_start_in_frames;
    m_impl->next_output_frame_index = output_start_frame_index;

    // The caller will pull upstream backward by this many frames, so the first
    // preroll_target frames pushed into push_block are pre-target history; we'll
    // pass them to seek() and discard them once the seek seeds the stretcher.
    m_impl->expected_next_input_media_frame = media_start_in_frames - m_impl->preroll_target;
    return static_cast<i64>(m_impl->preroll_target);
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
            int chunk = static_cast<int>(min<i64>(gap, m_impl->stride));
            m_impl->append_silence_to_input(chunk);
            gap -= chunk;
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

    auto remaining = frame_count - frames_to_skip;
    m_impl->append_interleaved_to_input(input.data().data(), frames_to_skip, remaining);
    m_impl->expected_next_input_media_frame = block_start + frame_count;
}

void SignalsmithTimeStretcher::signal_end_of_stream()
{
    m_impl->eos_signalled = true;
}

Media::DecoderErrorOr<Media::AudioBlock> SignalsmithTimeStretcher::retrieve_block()
{
    auto& impl = *m_impl;
    auto channel_count = impl.channel_count;

    // Phase 1: feed seek() once we have preroll_target frames of historical input.
    if (impl.preroll_pending) {
        if (impl.input_valid_count < impl.preroll_target) {
            return Media::DecoderError::with_description(
                Media::DecoderErrorCategory::NeedsMoreInput,
                "Time-stretcher needs preroll input"sv);
        }
        auto preroll_pointers = channel_pointers_in(impl.input_planar, impl.stride, channel_count);
        impl.stretch.seek(preroll_pointers, impl.preroll_target, static_cast<double>(impl.rate));
        // Drop the preroll frames; the library now owns them as history.
        impl.drop_input_front(impl.preroll_target);
        impl.preroll_pending = false;
    }

    // Phase 2: regular process().
    int output_count = impl.output_chunk_frames();
    int input_count = impl.input_frames_for_output(output_count);

    bool have_enough_input = impl.input_valid_count >= input_count;

    if (!have_enough_input) {
        if (impl.eos_signalled && !impl.eos_drained) {
            // Drain any remaining buffered output via flush(); subsequent calls
            // return NeedsMoreInput permanently for this stream.
            impl.eos_drained = true;
            int drain_frames = impl.stretch.outputLatency();
            if (drain_frames <= 0) {
                return Media::DecoderError::with_description(
                    Media::DecoderErrorCategory::NeedsMoreInput,
                    "Time-stretcher fully drained"sv);
            }
            // Resize output scratch if needed.
            if (static_cast<int>(impl.output_planar.size()) < drain_frames * channel_count)
                impl.output_planar.resize(static_cast<size_t>(drain_frames) * channel_count);
            auto output_pointers = channel_pointers_in(impl.output_planar, drain_frames, channel_count);
            impl.stretch.flush(output_pointers, drain_frames);

            // Stamp media-time using cumulative consumed input as the anchor.
            auto media_start_in_frames = impl.position_origin_media_frame + impl.input_consumed_post_seek;
            // The flushed tail is whatever remained in the analysis buffer; estimate
            // its media duration as drain_frames × rate (i.e. the input frames the
            // stretcher would have wanted to consume to produce these output frames).
            auto media_duration_in_frames = static_cast<i64>(std::lround(
                static_cast<double>(drain_frames) * static_cast<double>(impl.rate)));
            Media::AudioBlock out;
            if (!impl.latest_input_spec.has_value()) {
                return Media::DecoderError::with_description(
                    Media::DecoderErrorCategory::Invalid,
                    "Time-stretcher emitted output before any input was pushed"sv);
            }
            auto out_anchor = impl.next_output_frame_index;
            out.emplace(impl.latest_input_spec.value(), out_anchor, [&](Media::AudioBlock::Data& data) {
                data.resize_and_keep_capacity(static_cast<size_t>(drain_frames) * channel_count);
                for (int i = 0; i < drain_frames; ++i) {
                    for (int c = 0; c < channel_count; ++c) {
                        auto* channel_base = impl.output_planar.data() + (static_cast<ptrdiff_t>(c) * drain_frames);
                        data[(i * channel_count) + c] = channel_base[i];
                    }
                }
            });
            out.set_media_time_start(AK::Duration::from_time_units(media_start_in_frames, 1, impl.sample_rate));
            out.set_media_time_duration(AK::Duration::from_time_units(media_duration_in_frames, 1, impl.sample_rate));
            impl.next_output_frame_index += drain_frames;
            impl.input_consumed_post_seek += media_duration_in_frames;
            return out;
        }

        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::NeedsMoreInput,
            "Time-stretcher needs more input to produce output"sv);
    }

    if (!impl.latest_input_spec.has_value()) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::Invalid,
            "Time-stretcher emitted output before any input was pushed"sv);
    }

    auto input_pointers = channel_pointers_in(impl.input_planar, impl.stride, channel_count);
    auto output_pointers = channel_pointers_in(impl.output_planar, output_count, channel_count);
    impl.stretch.process(input_pointers, input_count, output_pointers, output_count);

    // Stamp the emitted block. media_time covers exactly the input frames the
    // stretcher just consumed; output frames live in the output-stream coordinate
    // system rooted at next_output_frame_index.
    auto media_start_in_frames = impl.position_origin_media_frame + impl.input_consumed_post_seek;
    auto media_duration_in_frames = static_cast<i64>(input_count);
    auto out_anchor = impl.next_output_frame_index;

    Media::AudioBlock out;
    out.emplace(impl.latest_input_spec.value(), out_anchor, [&](Media::AudioBlock::Data& data) {
        data.resize_and_keep_capacity(static_cast<size_t>(output_count) * channel_count);
        for (int i = 0; i < output_count; ++i) {
            for (int c = 0; c < channel_count; ++c) {
                auto* channel_base = impl.output_planar.data() + (static_cast<ptrdiff_t>(c) * output_count);
                data[(i * channel_count) + c] = channel_base[i];
            }
        }
    });
    out.set_media_time_start(AK::Duration::from_time_units(media_start_in_frames, 1, impl.sample_rate));
    out.set_media_time_duration(AK::Duration::from_time_units(media_duration_in_frames, 1, impl.sample_rate));

    impl.drop_input_front(input_count);
    impl.input_consumed_post_seek += input_count;
    impl.next_output_frame_index += output_count;
    return out;
}

}

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

#include "BungeeTimeStretcher.h"

namespace Audio {

static constexpr int LOG2_SYNTHESIS_HOP_ADJUST = -1;
static constexpr int INPUT_BUFFER_MARGIN_FRAMES = 8192;

struct BungeeTimeStretcher::Impl {
    int sample_rate;
    int channel_count;

    Bungee::Stretcher<Bungee::Basic> stretcher;

    Vector<float> planar_data;
    int plane_stride;
    i64 begin_position { 0 };
    int valid_count { 0 };

    Bungee::Request request {};
    Bungee::OutputChunk output_chunk {};
    bool output_chunk_valid { false };
    int output_chunk_consumed { 0 };

    float rate { 1.0f };
    float pitch { 1.0f };

    i64 position_origin_media_frame { 0 };
    i64 next_output_frame_index { 0 };
    i64 expected_next_input_bungee_position { 0 };
    bool eos_signalled { false };
    Optional<SampleSpecification> latest_input_spec;

    Impl(int sample_rate, int channel_count)
        : sample_rate(sample_rate)
        , channel_count(channel_count)
        , stretcher(Bungee::SampleRates { sample_rate, sample_rate }, channel_count, LOG2_SYNTHESIS_HOP_ADJUST)
        , plane_stride(stretcher.maxInputFrameCount() + INPUT_BUFFER_MARGIN_FRAMES)
    {
        planar_data.resize(static_cast<size_t>(plane_stride) * static_cast<size_t>(channel_count));
        request.position = std::numeric_limits<double>::quiet_NaN();
    }

    int half_input_frame_count() const
    {
        // Bungee's grain analysis chunk spans 2 * halfInputFrameCount input frames
        // (centered on request.position). At ratio = 1 (no pitch shift), this is half
        // the FFT window: 1 << (log2SynthesisHop + 3) >> 1 = 4 * synthesisHop. Per
        // Timing.cpp the synthesis hop is determined by sample rate; we approximate
        // halfInputFrameCount conservatively as maxInputFrameCount / 8 — small enough
        // never to under-allocate, large enough to be a meaningful right-side check.
        return stretcher.maxInputFrameCount() / 8;
    }

    void recreate_stretcher()
    {
        stretcher.~Stretcher();
        new (&stretcher) Bungee::Stretcher<Bungee::Basic>(
            Bungee::SampleRates { sample_rate, sample_rate },
            channel_count,
            LOG2_SYNTHESIS_HOP_ADJUST);
    }

    i64 end_position() const { return begin_position + valid_count; }
    int free_capacity() const { return plane_stride - valid_count; }

    void reset_input_buffer(i64 starting_bungee_position)
    {
        begin_position = starting_bungee_position;
        valid_count = 0;
    }

    void discard_up_to(i64 desired_begin)
    {
        if (desired_begin <= begin_position)
            return;
        auto discard = static_cast<int>(min<i64>(desired_begin - begin_position, valid_count));
        if (discard > 0) {
            for (int channel = 0; channel < channel_count; channel++) {
                auto* channel_base = planar_data.data() + (static_cast<size_t>(channel) * plane_stride);
                for (int i = 0; i < valid_count - discard; i++)
                    channel_base[i] = channel_base[i + discard];
            }
            valid_count -= discard;
            begin_position += discard;
        }
        if (desired_begin > begin_position) {
            begin_position = desired_begin;
            valid_count = 0;
        }
    }

    void append_silence(int frame_count)
    {
        VERIFY(frame_count >= 0);
        VERIFY(valid_count + frame_count <= plane_stride);
        for (int channel = 0; channel < channel_count; channel++) {
            auto* channel_base = planar_data.data() + (static_cast<size_t>(channel) * plane_stride);
            for (int i = 0; i < frame_count; i++)
                channel_base[valid_count + i] = 0.0f;
        }
        valid_count += frame_count;
    }

    void append_interleaved(ReadonlySpan<float> interleaved, int frame_offset, int frame_count)
    {
        VERIFY(frame_count >= 0);
        VERIFY(valid_count + frame_count <= plane_stride);
        for (int channel = 0; channel < channel_count; channel++) {
            auto* channel_base = planar_data.data() + (static_cast<size_t>(channel) * plane_stride);
            for (int i = 0; i < frame_count; i++)
                channel_base[valid_count + i] = interleaved[((frame_offset + i) * channel_count) + channel];
        }
        valid_count += frame_count;
    }

    i64 compute_preroll() const
    {
        Bungee::Request probe {};
        probe.position = 0.0;
        probe.speed = static_cast<double>(rate);
        probe.pitch = static_cast<double>(pitch);
        probe.reset = true;
        stretcher.preroll(probe);
        VERIFY(probe.position <= 0.0);
        return static_cast<i64>(-probe.position);
    }
};

ErrorOr<NonnullOwnPtr<TimeStretcher>> BungeeTimeStretcher::create(u32 sample_rate, u8 channel_count)
{
    if (sample_rate > NumericLimits<int>::max())
        return Error::from_string_literal("Sample rate is too large");

    auto impl = adopt_own(*new Impl(static_cast<int>(sample_rate), channel_count));
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

i64 BungeeTimeStretcher::flush_and_get_preroll(AK::Duration media_start_timestamp, i64 output_start_frame_index)
{
    m_impl->recreate_stretcher();
    auto preroll = m_impl->compute_preroll();

    auto media_start_in_frames = media_start_timestamp.to_time_units(1, m_impl->sample_rate);
    m_impl->position_origin_media_frame = media_start_in_frames;
    m_impl->next_output_frame_index = output_start_frame_index;

    m_impl->expected_next_input_bungee_position = -preroll;
    m_impl->reset_input_buffer(-preroll);

    // request.position = NaN signals retrieve_block that this is the first grain
    // after a flush. The first grain uses a position-from-end formula; subsequent
    // grains advance via Stretcher::next() which moves position by speed × unitHop —
    // exactly the per-grain hop Bungee expects to derive request.speed from.
    m_impl->request.position = std::numeric_limits<double>::quiet_NaN();
    m_impl->request.speed = static_cast<double>(m_impl->rate);
    m_impl->request.pitch = static_cast<double>(m_impl->pitch);
    m_impl->request.reset = true;

    m_impl->output_chunk = {};
    m_impl->output_chunk_valid = false;
    m_impl->output_chunk_consumed = 0;
    m_impl->eos_signalled = false;

    return preroll;
}

void BungeeTimeStretcher::push_block(Media::AudioBlock const& input)
{
    VERIFY(!input.is_empty());
    VERIFY(input.channel_count() == m_impl->channel_count);
    VERIFY(static_cast<int>(input.sample_rate()) == m_impl->sample_rate);

    // Input must be unscaled — chained stretchers are not supported.
    auto expected_start = AK::Duration::from_time_units(input.first_frame_index(), 1, m_impl->sample_rate);
    auto expected_duration = AK::Duration::from_time_units(static_cast<i64>(input.frame_count()), 1, m_impl->sample_rate);
    if (input.media_time_start() != expected_start)
        dbgln("unexpected media time start: {} != {}", input.media_time_start(), expected_start);
    if (input.media_time_duration() != expected_duration)
        dbgln("unexpected media time duration: {} != {}, frame count {}, {} == {}?", input.media_time_duration(), expected_duration, input.frame_count(), m_impl->sample_rate, input.sample_rate());

    VERIFY(input.media_time_start() == expected_start);
    VERIFY(input.media_time_duration() == expected_duration);

    m_impl->latest_input_spec = input.sample_specification();

    auto block_bungee_start = input.first_frame_index() - m_impl->position_origin_media_frame;
    auto frame_count = static_cast<int>(input.frame_count());
    int frames_to_skip = 0;

    auto gap = block_bungee_start - m_impl->expected_next_input_bungee_position;
    if (gap > 0) {
        while (gap > 0) {
            auto room = m_impl->free_capacity();
            if (room <= 0) {
                m_impl->discard_up_to(m_impl->begin_position + 1);
                continue;
            }
            auto chunk = static_cast<int>(min<i64>(gap, room));
            m_impl->append_silence(chunk);
            gap -= chunk;
        }
    } else if (gap < 0) {
        frames_to_skip = static_cast<int>(min<i64>(-gap, frame_count));
        if (frames_to_skip == frame_count) {
            m_impl->expected_next_input_bungee_position = max(
                m_impl->expected_next_input_bungee_position,
                block_bungee_start + frame_count);
            return;
        }
    }

    auto remaining = frame_count - frames_to_skip;
    auto offset = frames_to_skip;
    while (remaining > 0) {
        auto room = m_impl->free_capacity();
        if (room <= 0) {
            m_impl->discard_up_to(m_impl->begin_position + 1);
            continue;
        }
        auto chunk = min(remaining, room);
        m_impl->append_interleaved(input.data(), offset, chunk);
        offset += chunk;
        remaining -= chunk;
    }
    m_impl->expected_next_input_bungee_position = block_bungee_start + frame_count;
}

void BungeeTimeStretcher::signal_end_of_stream()
{
    m_impl->eos_signalled = true;
}

Media::DecoderErrorOr<Media::AudioBlock> BungeeTimeStretcher::retrieve_block()
{
    auto const channel_count = static_cast<size_t>(m_impl->channel_count);
    auto const half_input_frame_count = m_impl->half_input_frame_count();

    auto out_anchor_in_frames = m_impl->next_output_frame_index;
    Vector<float> emitted_interleaved;
    size_t emitted_frame_count = 0;

    // Bungee positions covering the slice of the chunk that this call emitted.
    // Used below to derive the output block's media_time_start/duration.
    double emit_position_start_bungee = 0.0;
    double emit_position_end_bungee = 0.0;
    bool emit_positions_captured = false;

    while (emitted_frame_count == 0) {
        // Drain the current output chunk if there's anything left in it.
        if (m_impl->output_chunk_valid) {
            auto const& chunk = m_impl->output_chunk;
            int remaining_in_chunk = chunk.frameCount - m_impl->output_chunk_consumed;
            if (remaining_in_chunk > 0 && chunk.request[0] != nullptr && !std::isnan(chunk.request[0]->position) && chunk.request[1] != nullptr && !std::isnan(chunk.request[1]->position)) {
                double chunk_start_position = chunk.request[0]->position;
                double chunk_end_position = chunk.request[1]->position;
                double position_step = chunk.frameCount > 0
                    ? (chunk_end_position - chunk_start_position) / static_cast<double>(chunk.frameCount)
                    : 0.0;

                int discard_frames = 0;
                double position_at_consumed = chunk_start_position
                    + (position_step * m_impl->output_chunk_consumed);
                if (position_at_consumed < 0.0 && position_step > 0.0) {
                    double frames_until_zero = -position_at_consumed / position_step;
                    discard_frames = min(remaining_in_chunk, static_cast<int>(AK::ceil(frames_until_zero)));
                    discard_frames = max(0, discard_frames);
                }
                if (discard_frames > 0) {
                    m_impl->output_chunk_consumed += discard_frames;
                    remaining_in_chunk -= discard_frames;
                }

                if (remaining_in_chunk > 0) {
                    emit_position_start_bungee = chunk_start_position + (position_step * static_cast<double>(m_impl->output_chunk_consumed));
                    emit_position_end_bungee = chunk_end_position;
                    emit_positions_captured = true;

                    auto base_offset = emitted_interleaved.size();
                    emitted_interleaved.resize(base_offset + (remaining_in_chunk * channel_count));
                    for (int i = 0; i < remaining_in_chunk; ++i) {
                        auto src_idx = m_impl->output_chunk_consumed + i;
                        for (size_t c = 0; c < channel_count; ++c) {
                            auto src = chunk.data[src_idx + (c * chunk.channelStride)];
                            emitted_interleaved[base_offset + (i * channel_count) + c] = src;
                        }
                    }
                    emitted_frame_count += remaining_in_chunk;
                    m_impl->output_chunk_consumed += remaining_in_chunk;
                }
            }
            if (m_impl->output_chunk_consumed >= m_impl->output_chunk.frameCount)
                m_impl->output_chunk_valid = false;

            if (emitted_frame_count > 0)
                break;
        }

        // Compute the next request.position. For the very first grain after flush,
        // anchor near end_position so the chunk fits in our buffer; for subsequent
        // grains, advance via Stretcher::next() which moves position by exactly
        // request.speed × unitHop — that's the per-grain hop Bungee uses to
        // determine the actual stretch factor (Grain.cpp:35-47).
        auto saved_position = m_impl->request.position;
        if (std::isnan(saved_position)) {
            m_impl->request.position = static_cast<double>(m_impl->end_position()) - static_cast<double>(half_input_frame_count);
            m_impl->request.reset = true;
        } else {
            m_impl->stretcher.next(m_impl->request);
            m_impl->request.reset = false;
        }
        m_impl->request.speed = static_cast<double>(m_impl->rate);
        m_impl->request.pitch = static_cast<double>(m_impl->pitch);

        if (m_impl->eos_signalled && m_impl->valid_count == 0)
            m_impl->request.position = std::numeric_limits<double>::quiet_NaN();

        if (std::isnan(m_impl->request.position) && m_impl->stretcher.isFlushed())
            break;

        // Pre-check buffer coverage. If the chunk Bungee will request extends past
        // what we have, restore position and bail — TSP will push more input and try
        // again on the next retrieve_block call. This is the only safe place to
        // break: once we've called specifyGrain, Bungee's instrumentation requires
        // analyseGrain + synthesiseGrain to follow.
        auto expected_chunk_end = static_cast<i64>(std::round(m_impl->request.position)) + half_input_frame_count;
        if (m_impl->end_position() < expected_chunk_end && !m_impl->eos_signalled) {
            m_impl->request.position = saved_position;
            break;
        }

        // Trio (must be called in this exact order):
        auto input_chunk = m_impl->stretcher.specifyGrain(m_impl->request);

        m_impl->discard_up_to(input_chunk.begin);

        int mute_head = static_cast<int>(max<i64>(0, m_impl->begin_position - input_chunk.begin));
        int mute_tail = static_cast<int>(max<i64>(0, input_chunk.end - m_impl->end_position()));
        auto* analyse_data = m_impl->planar_data.data() - mute_head;
        m_impl->stretcher.analyseGrain(analyse_data, m_impl->plane_stride, mute_head, mute_tail);
        m_impl->stretcher.synthesiseGrain(m_impl->output_chunk);
        m_impl->output_chunk_valid = true;
        m_impl->output_chunk_consumed = 0;

        if (m_impl->eos_signalled && m_impl->stretcher.isFlushed())
            break;
    }

    if (emitted_frame_count == 0) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::NeedsMoreInput,
            "Time-stretcher needs more input to produce output"sv);
    }

    if (!m_impl->latest_input_spec.has_value()) {
        return Media::DecoderError::with_description(
            Media::DecoderErrorCategory::Invalid,
            "Time-stretcher emitted output before any input was pushed"sv);
    }

    VERIFY(emit_positions_captured);

    Media::AudioBlock out;
    out.emplace(m_impl->latest_input_spec.value(), out_anchor_in_frames, [&](Media::AudioBlock::Data& data) {
        data.resize_and_keep_capacity(emitted_interleaved.size());
        AK::TypedTransfer<float>::copy(data.data(), emitted_interleaved.data(), emitted_interleaved.size());
    });

    // Map the captured Bungee positions back into media frames and stamp the block.
    // Bungee's request[0]/request[1] pointers alias slots in a rotating grain queue,
    // so consecutive blocks chain by identity — no cumulative tracking is needed.
    auto start_in_media_frames = m_impl->position_origin_media_frame + static_cast<i64>(std::round(emit_position_start_bungee));
    auto end_in_media_frames = m_impl->position_origin_media_frame + static_cast<i64>(std::round(emit_position_end_bungee));
    out.set_media_time_start(AK::Duration::from_time_units(start_in_media_frames, 1, m_impl->sample_rate));
    out.set_media_time_duration(AK::Duration::from_time_units(end_in_media_frames - start_in_media_frames, 1, m_impl->sample_rate));

    m_impl->next_output_frame_index += static_cast<i64>(emitted_frame_count);
    return out;
}

}

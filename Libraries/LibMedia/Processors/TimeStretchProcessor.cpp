/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/BungeeTimeStretcher.h>
#include <LibMedia/Processors/TimeStretchProcessor.h>

namespace Media {

ErrorOr<NonnullRefPtr<TimeStretchProcessor>> TimeStretchProcessor::try_create()
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) TimeStretchProcessor);
}

TimeStretchProcessor::TimeStretchProcessor() = default;

ErrorOr<void> TimeStretchProcessor::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Threading::MutexLocker locker { m_mutex };
    VERIFY(m_input == nullptr);
    m_input = input;
    input->set_state_changed_handler([self = NonnullRefPtr(*this)](PipelineStatus status) {
        if (self->m_state_changed_handler)
            self->m_state_changed_handler(status);
    });
    if (m_sample_specification.is_valid()) {
        if (auto result = input->set_output_sample_specification(m_sample_specification); result.is_error()) {
            input->set_state_changed_handler(nullptr);
            m_input = nullptr;
            return result.release_error();
        }
        if (m_started)
            input->start();
    }
    return {};
}

void TimeStretchProcessor::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    Threading::MutexLocker locker { m_mutex };
    VERIFY(m_input == input);
    input->set_state_changed_handler(nullptr);
    m_input = nullptr;
}

void TimeStretchProcessor::seek(AK::Duration timestamp)
{
    Threading::MutexLocker locker { m_mutex };

    m_next_emit_media_time = timestamp;
    if (m_sample_specification.is_valid())
        m_next_output_frame = timestamp.to_time_units(1, m_sample_specification.sample_rate());
    m_upstream_eos_signalled = false;

    auto rate = m_rate.load();
    auto upstream_timestamp = timestamp;
    if (rate != 1.0f && m_stretcher) {
        m_stretcher->set_rate(rate);
        auto preroll = m_stretcher->flush_and_get_preroll(timestamp, m_next_output_frame);
        if (m_sample_specification.is_valid()) {
            auto preroll_offset = AK::Duration::from_time_units(preroll, 1, m_sample_specification.sample_rate());
            upstream_timestamp = max(AK::Duration::zero(), timestamp - preroll_offset);
        }
        m_mode = Mode::Stretcher;
    } else {
        m_mode = Mode::FastPath;
    }

    if (m_input != nullptr)
        m_input->seek(upstream_timestamp);
}

ErrorOr<void> TimeStretchProcessor::set_output_sample_specification(Audio::SampleSpecification sample_specification)
{
    Threading::MutexLocker locker { m_mutex };
    if (m_sample_specification == sample_specification)
        return {};
    m_sample_specification = sample_specification;
    m_stretcher = nullptr;
    if (m_input != nullptr)
        TRY(m_input->set_output_sample_specification(sample_specification));
    return {};
}

void TimeStretchProcessor::start()
{
    Threading::MutexLocker locker { m_mutex };
    m_started = true;
    if (m_input != nullptr)
        m_input->start();
}

void TimeStretchProcessor::set_state_changed_handler(PipelineStateChangeHandler handler)
{
    Threading::MutexLocker locker { m_mutex };
    m_state_changed_handler = move(handler);
}

ErrorOr<void> TimeStretchProcessor::set_stretch(float rate)
{
    if (rate <= 0.0f)
        return Error::from_string_literal("Time-stretch rate must be positive");
    m_rate = rate;
    Threading::MutexLocker locker { m_mutex };
    if (m_stretcher)
        m_stretcher->set_rate(rate);
    return {};
}

void TimeStretchProcessor::ensure_stretcher_while_locked()
{
    if (m_stretcher)
        return;
    if (!m_sample_specification.is_valid())
        return;
    auto maybe_stretcher = Audio::BungeeTimeStretcher::create(m_sample_specification.sample_rate(), m_sample_specification.channel_count());
    if (maybe_stretcher.is_error())
        return;
    m_stretcher = maybe_stretcher.release_value();
    m_stretcher->set_rate(m_rate);
}

PipelineStatus TimeStretchProcessor::pull(AudioBlock& into)
{
    /*auto start_time = MonotonicTime::now();
    ScopeGuard print_time = [&] {
        auto end_time = MonotonicTime::now();
        dbgln("TimeStretchProcessor::pull() took {}", end_time - start_time);
    };*/

    Threading::MutexLocker locker { m_mutex };
    if (m_input == nullptr || !m_sample_specification.is_valid())
        return PipelineStatus::Pending;

    auto rate = m_rate.load();

    if (rate != 1.0f)
        ensure_stretcher_while_locked();

    if (rate == 1.0f || m_stretcher == nullptr) {
        if (m_mode == Mode::Stretcher) {
            m_input->seek(m_next_emit_media_time);
            m_mode = Mode::FastPath;
        }

        AudioBlock input_block;
        auto status = m_input->pull(input_block);
        if (input_block.is_empty())
            return status;
        VERIFY(can_carry_data(status));
        input_block.set_first_frame_index(m_next_output_frame);
        into = move(input_block);
        m_next_output_frame = into.end_frame_index();
        m_next_emit_media_time = into.media_time_end();
        return status;
    }

    if (m_mode == Mode::FastPath) {
        m_stretcher->set_rate(rate);
        auto preroll = m_stretcher->flush_and_get_preroll(m_next_emit_media_time, m_next_output_frame);
        auto preroll_offset = AK::Duration::from_time_units(preroll, 1, m_sample_specification.sample_rate());
        auto upstream_target = max(AK::Duration::zero(), m_next_emit_media_time - preroll_offset);
        m_input->seek(upstream_target);
        m_mode = Mode::Stretcher;
    }

    m_stretcher->set_rate(rate);

    while (true) {
        auto result = m_stretcher->retrieve_block();
        if (!result.is_error()) {
            into = result.release_value();
            m_next_output_frame = into.end_frame_index();
            m_next_emit_media_time = into.media_time_end();
            return PipelineStatus::HaveData;
        }
        if (result.error().category() != DecoderErrorCategory::NeedsMoreInput)
            return PipelineStatus::Error;

        if (m_upstream_eos_signalled)
            return PipelineStatus::EndOfStream;

        AudioBlock input_block;
        auto status = m_input->pull(input_block);
        if (input_block.is_empty()) {
            if (status == PipelineStatus::EndOfStream) {
                m_stretcher->signal_end_of_stream();
                m_upstream_eos_signalled = true;
                continue;
            }
            return status;
        }
        VERIFY(can_carry_data(status));
        m_stretcher->push_block(input_block);
    }
}

}

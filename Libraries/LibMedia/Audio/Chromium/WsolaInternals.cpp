// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported to Ladybird (BSD-2-Clause). Original copyright preserved per BSD-3.
// Source: chromium/media/filters/wsola_internals.{h,cc}.

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <cmath>
#include <numbers>

#include "AudioBus.h"
#include "WsolaInternals.h"

namespace Audio::Chromium::WsolaInternals {

namespace {

bool in_interval(size_t n, Interval interval)
{
    return n >= interval.low && n <= interval.high;
}

float multi_channel_similarity_measure(ReadonlySpan<float> dot_prod_a_b,
    ReadonlySpan<float> energy_a, ReadonlySpan<float> energy_b)
{
    VERIFY(dot_prod_a_b.size() == energy_a.size());
    VERIFY(energy_a.size() == energy_b.size());
    constexpr float epsilon = 1e-12f;
    float similarity_measure = 0.0f;
    for (size_t n = 0; n < dot_prod_a_b.size(); ++n)
        similarity_measure += dot_prod_a_b[n] / std::sqrt((energy_a[n] * energy_b[n]) + epsilon);
    return similarity_measure;
}

void multi_channel_dot_product_c(AudioBus const& a, size_t frame_offset_a,
    AudioBus const& b, size_t frame_offset_b,
    size_t num_frames, Span<float> dot_product)
{
    VERIFY(static_cast<int>(dot_product.size()) == a.channels());
    for (int channel_index = 0; channel_index < a.channels(); ++channel_index) {
        auto channel_a = a.channel(channel_index).slice(frame_offset_a, num_frames);
        auto channel_b = b.channel(channel_index).slice(frame_offset_b, num_frames);
        float sum = 0.0f;
        for (size_t i = 0; i < num_frames; ++i)
            sum += channel_a[i] * channel_b[i];
        dot_product[channel_index] = sum;
    }
}

}

void multi_channel_dot_product(AudioBus const& a, size_t frame_offset_a,
    AudioBus const& b, size_t frame_offset_b,
    size_t num_frames, Span<float> dot_product)
{
    VERIFY(a.channels() == b.channels());
    VERIFY(static_cast<int>(dot_product.size()) == a.channels());
    VERIFY(frame_offset_a + num_frames <= static_cast<size_t>(a.frames()));
    VERIFY(frame_offset_b + num_frames <= static_cast<size_t>(b.frames()));
    // SIMD variants exist in Chromium but the C version is correct everywhere
    // and adequate for first-cut performance.
    multi_channel_dot_product_c(a, frame_offset_a, b, frame_offset_b, num_frames, dot_product);
}

void multi_channel_moving_block_energies(AudioBus const& input,
    size_t frames_per_window, Span<float> energy)
{
    auto num_blocks = static_cast<size_t>(input.frames()) - (frames_per_window - 1);
    auto channel_count = static_cast<size_t>(input.channels());
    VERIFY(energy.size() == num_blocks * channel_count);

    for (int channel_index = 0; channel_index < input.channels(); ++channel_index) {
        auto input_channel = input.channel(channel_index);

        // Energy of the first block.
        float first_block_energy = 0.0f;
        for (size_t i = 0; i < frames_per_window; ++i)
            first_block_energy += input_channel[i] * input_channel[i];
        energy[channel_index] = first_block_energy;

        // Slide the block by one frame at a time, subtracting the leaving
        // sample's energy and adding the entering sample's.
        for (size_t block_index = 1; block_index < num_blocks; ++block_index) {
            auto leaving_sample = input_channel[block_index - 1];
            auto entering_sample = input_channel[block_index + frames_per_window - 1];
            energy[channel_index + (block_index * channel_count)] =
                energy[channel_index + ((block_index - 1) * channel_count)]
                - (leaving_sample * leaving_sample)
                + (entering_sample * entering_sample);
        }
    }
}

void quadratic_interpolation(ReadonlySpan<float> y_values,
    float& extremum, float& extremum_value)
{
    VERIFY(y_values.size() == 3);
    float const a = (0.5f * (y_values[2] + y_values[0])) - y_values[1];
    float const b = 0.5f * (y_values[2] - y_values[0]);
    float const c = y_values[1];

    if (a == 0.0f) {
        extremum = 0.0f;
        extremum_value = y_values[1];
    } else {
        float const ext = -b / (2.0f * a);
        extremum = ext;
        extremum_value = (a * ext * ext) + (b * ext) + c;
    }
}

size_t decimated_search(size_t decimation, Interval exclude_interval,
    AudioBus const& target_block, AudioBus const& search_segment,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks)
{
    auto channel_count = static_cast<size_t>(search_segment.channels());
    auto block_size = static_cast<size_t>(target_block.frames());
    auto num_candidate_blocks = static_cast<size_t>(search_segment.frames()) - (block_size - 1);
    Vector<float> dot_product;
    dot_product.resize(channel_count);
    float similarity[3]; // For cubic interpolation.

    size_t n = 0;
    multi_channel_dot_product(target_block, 0, search_segment, n, block_size, dot_product.span());
    similarity[0] = multi_channel_similarity_measure(
        dot_product.span(), energy_target_block,
        energy_candidate_blocks.slice(n * channel_count, channel_count));

    float best_similarity = similarity[0];
    size_t optimal_block_index = 0;

    n += decimation;
    if (n >= num_candidate_blocks)
        return 0;

    multi_channel_dot_product(target_block, 0, search_segment, n, block_size, dot_product.span());
    similarity[1] = multi_channel_similarity_measure(
        dot_product.span(), energy_target_block,
        energy_candidate_blocks.slice(n * channel_count, channel_count));

    n += decimation;
    if (n >= num_candidate_blocks) {
        // Only two samples available — return the better.
        return similarity[1] > similarity[0] ? decimation : 0;
    }

    for (; n < num_candidate_blocks; n += decimation) {
        multi_channel_dot_product(target_block, 0, search_segment, n, block_size, dot_product.span());
        similarity[2] = multi_channel_similarity_measure(
            dot_product.span(), energy_target_block,
            energy_candidate_blocks.slice(n * channel_count, channel_count));

        bool is_local_max = (similarity[1] > similarity[0] && similarity[1] >= similarity[2])
            || (similarity[1] >= similarity[0] && similarity[1] > similarity[2]);

        if (is_local_max) {
            float normalized_candidate_index;
            float candidate_similarity;
            quadratic_interpolation({ similarity, 3 }, normalized_candidate_index, candidate_similarity);

            int candidate_index = static_cast<int>(n - decimation)
                + AK::round_to<int>(normalized_candidate_index * static_cast<float>(decimation));
            if (candidate_similarity > best_similarity
                && !in_interval(static_cast<size_t>(candidate_index), exclude_interval)) {
                optimal_block_index = static_cast<size_t>(candidate_index);
                best_similarity = candidate_similarity;
            }
        } else if (n + decimation >= num_candidate_blocks
            && similarity[2] > best_similarity
            && !in_interval(n, exclude_interval)) {
            // Endpoint with higher similarity than current best.
            optimal_block_index = n;
            best_similarity = similarity[2];
        }

        similarity[0] = similarity[1];
        similarity[1] = similarity[2];
    }
    return optimal_block_index;
}

size_t full_search(size_t low_limit, size_t high_limit, Interval exclude_interval,
    AudioBus const& target_block, AudioBus const& search_block,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks)
{
    auto channel_count = static_cast<size_t>(search_block.channels());
    auto block_size = static_cast<size_t>(target_block.frames());
    Vector<float> dot_product;
    dot_product.resize(channel_count);

    float best_similarity = NumericLimits<float>::lowest();
    size_t optimal_block_index = 0;

    for (size_t n = low_limit; n <= high_limit; ++n) {
        if (in_interval(n, exclude_interval))
            continue;
        multi_channel_dot_product(target_block, 0, search_block, n, block_size, dot_product.span());
        float similarity = multi_channel_similarity_measure(
            dot_product.span(), energy_target_block,
            energy_candidate_blocks.slice(n * channel_count, channel_count));
        if (similarity > best_similarity) {
            best_similarity = similarity;
            optimal_block_index = n;
        }
    }
    return optimal_block_index;
}

size_t optimal_index(AudioBus const& search_block, AudioBus const& target_block,
    Interval exclude_interval)
{
    VERIFY(search_block.channels() == target_block.channels());
    auto channel_count = static_cast<size_t>(search_block.channels());
    auto target_size = static_cast<size_t>(target_block.frames());
    auto num_candidate_blocks = static_cast<size_t>(search_block.frames()) - (target_size - 1);

    // Compromise between complexity reduction and search accuracy. Chromium's
    // comment notes this was chosen heuristically.
    constexpr size_t search_decimation = 5;

    Vector<float> energy_target_block;
    energy_target_block.resize(channel_count);
    Vector<float> energy_candidate_blocks;
    energy_candidate_blocks.resize(channel_count * num_candidate_blocks);

    multi_channel_moving_block_energies(search_block, target_size, energy_candidate_blocks.span());
    multi_channel_dot_product(target_block, 0, target_block, 0, target_size, energy_target_block.span());

    size_t coarse_index = decimated_search(
        search_decimation, exclude_interval, target_block, search_block,
        energy_target_block.span(), energy_candidate_blocks.span());

    size_t low_limit = coarse_index < search_decimation ? 0u : coarse_index - search_decimation;
    size_t high_limit = min<size_t>(num_candidate_blocks - 1, coarse_index + search_decimation);
    return full_search(low_limit, high_limit, exclude_interval, target_block, search_block,
        energy_target_block.span(), energy_candidate_blocks.span());
}

void get_periodic_hanning_window(Span<float> window)
{
    float const scale = 2.0f * std::numbers::pi_v<float> / static_cast<float>(window.size());
    for (size_t n = 0; n < window.size(); ++n)
        window[n] = 0.5f * (1.0f - std::cos(static_cast<float>(n) * scale));
}

}

// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported to Ladybird (BSD-2-Clause). Original copyright preserved per BSD-3.
// Source: chromium/media/filters/wsola_internals.{h,cc}.

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>

namespace Audio::Chromium {
class AudioBus;
}

namespace Audio::Chromium::WsolaInternals {

struct Interval {
    size_t low;
    size_t high;
};

// Dot-product of channels of two AudioBus. For each AudioBus an offset is
// given. dot_product[k] is the dot-product of channel k. The caller should
// allocate sufficient space for dot_product.
void multi_channel_dot_product(AudioBus const& a, size_t frame_offset_a,
    AudioBus const& b, size_t frame_offset_b,
    size_t num_frames, Span<float> dot_product);

// Energies of sliding windows of channels are interleaved.
// The number of windows is input.frames() - (frames_per_window - 1), hence
// energy must be at least (input.frames() - (frames_per_window - 1)) * input.channels().
void multi_channel_moving_block_energies(AudioBus const& input,
    size_t frames_per_window, Span<float> energy);

// Fit f(x) = a*x^2 + b*x + c such that f(-1)=y[0], f(0)=y[1], f(1)=y[2] and
// return the maximum, assuming y[0] <= y[1] >= y[2].
void quadratic_interpolation(ReadonlySpan<float> y_values,
    float& extremum, float& extremum_value);

// Search a subset of all candidate blocks. The search is performed every
// `decimation` frames. A cubic interpolation gives a better estimate of the best match.
size_t decimated_search(size_t decimation, Interval exclude_interval,
    AudioBus const& target_block, AudioBus const& search_segment,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks);

// Exhaustively search [low_limit, high_limit] of search_segment for the block
// most similar to target_block.
size_t full_search(size_t low_limit, size_t high_limit, Interval exclude_interval,
    AudioBus const& target_block, AudioBus const& search_block,
    ReadonlySpan<float> energy_target_block,
    ReadonlySpan<float> energy_candidate_blocks);

// Find the index of the block within search_block that is most similar to
// target_block. exclude_interval is excluded from the search.
size_t optimal_index(AudioBus const& search_block, AudioBus const& target_block,
    Interval exclude_interval);

// Periodic Hann window — first L samples of an L+1 Hann window. Perfect
// reconstruction for overlap-and-add at 50% overlap.
void get_periodic_hanning_window(Span<float> window);

}

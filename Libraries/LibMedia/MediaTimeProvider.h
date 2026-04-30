/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/MediaPipelineNode.h>

namespace Media {

class MediaTimeProvider : public virtual MediaPipelineNode {
public:
    virtual ~MediaTimeProvider() = default;

    virtual AK::Duration current_time() const = 0;
    virtual void resume() = 0;
    virtual void pause() = 0;
    virtual void seek(AK::Duration) = 0;

    // Rate is a positive multiplier (1.0 = normal speed). Concrete providers may reject rates
    // they cannot support (e.g. an audio-driven provider whose pipeline contains no time-stretcher).
    virtual ErrorOr<void> set_playback_rate(float rate) = 0;
};

}

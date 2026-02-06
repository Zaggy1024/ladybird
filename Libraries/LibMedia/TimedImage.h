/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibGfx/Forward.h>

namespace Media {

class TimedImage final {
public:
    TimedImage(AK::Duration timestamp, AK::Duration duration, NonnullRefPtr<Gfx::ImmutableBitmap>&& image);
    TimedImage();
    ~TimedImage();

    bool is_valid() const { return m_image != nullptr; }
    AK::Duration timestamp() const;
    AK::Duration duration() const;
    AK::Duration end() const;
    NonnullRefPtr<Gfx::ImmutableBitmap> image() const;
    NonnullRefPtr<Gfx::ImmutableBitmap> release_image();
    void clear();

private:
    AK::Duration m_timestamp;
    AK::Duration m_duration;
    RefPtr<Gfx::ImmutableBitmap> m_image;
};

}

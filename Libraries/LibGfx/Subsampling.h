/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Gfx {

enum class Subsampling : u8 {
    Y4CbCr44,
    Y4CbCr40,
    Y4CbCr22,
    Y4CbCr20,
    Y4CbCr11,
};

}

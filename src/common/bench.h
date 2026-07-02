// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

namespace Common {

// Benchmark / closed-loop testing mode, enabled via the SHAD_BENCH environment
// variable. When active, the emulator avoids interfering with a concurrent
// normal session: it does not grab controllers, does not steal window focus,
// runs windowed, and uses a distinct window title ("bench shadPS4"). The
// automated perf harness sets SHAD_BENCH; normal launches never do.
inline bool IsBenchmarkMode() {
    static const bool enabled = std::getenv("SHAD_BENCH") != nullptr;
    return enabled;
}

} // namespace Common

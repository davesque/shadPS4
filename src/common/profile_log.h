// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Common {

// Lightweight, env-gated profiling log for the perf harness. When the SHAD_PROFILE
// environment variable points at a file, every call appends a line:
//   <elapsed_ms_since_first_call> <event> <value_ms>
// across all threads with a single steady clock, so frame flips and the candidate
// stutter sources (shader translation, pipeline creation) can be correlated to
// attribute each frame-time spike to its real cause. A no-op when SHAD_PROFILE is
// unset (just a null-pointer check).
inline void ProfileLog(const char* event, double value_ms) {
    static std::FILE* file = []() -> std::FILE* {
        const char* path = std::getenv("SHAD_PROFILE");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    if (file == nullptr) {
        return;
    }
    static const auto start = std::chrono::steady_clock::now();
    static std::mutex mtx;
    const double t =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::scoped_lock lk{mtx};
    std::fprintf(file, "%.3f %s %.3f\n", t, event, value_ms);
    std::fflush(file);
}

// RAII timer: logs the wall time of the enclosing scope under `event`.
struct ScopeProfile {
    const char* event;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit ScopeProfile(const char* event_) : event{event_} {}
    ~ScopeProfile() {
        ProfileLog(event,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                       .count());
    }
};

} // namespace Common

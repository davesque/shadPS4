// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#include "common/present_log.h"
#include "common/stutter_log.h"
#include "common/types.h"

namespace Common {

namespace {

constexpr int N = static_cast<int>(StutterCat::Count);

// Running per-category totals (microseconds), summed across all threads.
std::array<std::atomic<u64>, N> g_accum_us{};

std::FILE* StutterFile() {
    static std::FILE* file = []() -> std::FILE* {
        const char* p = std::getenv("SHAD_STUTTER_LOG");
        if (p == nullptr || *p == '\0') {
            return nullptr;
        }
        RotateSessionLogs(std::filesystem::path{p});
        return std::fopen(p, "w");
    }();
    return file;
}

double Threshold() {
    static const double ms = []() -> double {
        const char* t = std::getenv("SHAD_STUTTER_MS");
        return (t != nullptr && *t != '\0') ? std::atof(t) : 24.0;
    }();
    return ms;
}

const char* Name(int i) {
    switch (static_cast<StutterCat>(i)) {
    case StutterCat::Translate:
        return "translate";
    case StutterCat::Pipeline:
        return "pipeline";
    case StutterCat::TexUpload:
        return "tex";
    case StutterCat::BufUpload:
        return "buf";
    case StutterCat::ImgCreate:
        return "imgcreate";
    case StutterCat::GpuWait:
        return "wait";
    case StutterCat::FileIo:
        return "fileio";
    default:
        return "?";
    }
}

} // namespace

void StutterAdd(StutterCat cat, double ms) {
    if (StutterFile() == nullptr) {
        return;
    }
    g_accum_us[static_cast<int>(cat)].fetch_add(static_cast<u64>(ms * 1000.0 + 0.5),
                                                std::memory_order_relaxed);
}

void StutterFrame(double frame_ms) {
    std::FILE* file = StutterFile();
    if (file == nullptr) {
        return;
    }
    static std::array<u64, N> last{};
    static std::mutex mtx;
    static const auto start = std::chrono::steady_clock::now();

    std::scoped_lock lk{mtx};
    // Advance bookkeeping every frame so deltas mean "since the previous frame".
    std::array<u64, N> delta{};
    u64 accounted_us = 0;
    for (int i = 0; i < N; ++i) {
        const u64 cur = g_accum_us[i].load(std::memory_order_relaxed);
        delta[i] = cur - last[i];
        last[i] = cur;
        accounted_us += delta[i];
    }
    if (frame_ms < Threshold()) {
        return;
    }
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double unaccounted = frame_ms - static_cast<double>(accounted_us) / 1000.0;
    std::fprintf(file, "%.2f stutter %.1f", t, frame_ms);
    for (int i = 0; i < N; ++i) {
        std::fprintf(file, " %s=%.1f", Name(i), static_cast<double>(delta[i]) / 1000.0);
    }
    std::fprintf(file, " unaccounted=%.1f\n", unaccounted < 0.0 ? 0.0 : unaccounted);
    std::fflush(file);
}

} // namespace Common

// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "common/present_log.h"
#include "common/types.h"

namespace Common {

void RotateSessionLogs(const std::filesystem::path& path) {
    constexpr int kRotatedSessions = 9;
    std::error_code ec;
    const auto numbered = [&path](int k) {
        std::filesystem::path p = path;
        p += "." + std::to_string(k);
        return p;
    };
    std::filesystem::remove(numbered(kRotatedSessions), ec);
    for (int k = kRotatedSessions - 1; k >= 1; --k) {
        std::filesystem::rename(numbered(k), numbered(k + 1), ec);
    }
    std::filesystem::rename(path, numbered(1), ec);
}

namespace {

constexpr int N = static_cast<int>(PresentStage::Count);

std::FILE* PresentFile() {
    static std::FILE* file = []() -> std::FILE* {
        const char* p = std::getenv("SHAD_PRESENT_LOG");
        if (p == nullptr || *p == '\0') {
            return nullptr;
        }
        RotateSessionLogs(std::filesystem::path{p});
        return std::fopen(p, "w");
    }();
    return file;
}

// Cross-thread running totals (guest videoout + GPU-emu threads).
std::atomic<u64> g_submit_count{0};  // guest flip submissions
std::atomic<u64> g_gpuwait_us{0};    // CPU-blocked-on-GPU microseconds
std::atomic<u64> g_gpubusy_us{0};    // GPU-busy microseconds (timestamp queries)
// Snapshots taken at the last per-second emit, so we can diff.
u64 g_submit_last = 0;
u64 g_gpuwait_last = 0;
u64 g_gpubusy_last = 0;

// Latest per-second summary, published for an on-screen readout. Written on the
// present thread, read on the (same) present/ImGui thread; plain is fine.
PresentOnscreen g_onscreen{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, false, false};

// Rolling history behind the on-screen sparkline charts (same threading).
PresentSeries g_series{};

// Active phase-parking detection: consecutive seconds the tick has spent within
// kPhaseBandMs of a vblank boundary. Phase moves at ppm speed, so per-second
// sampling can't miss a parking episode. Present thread only.
constexpr double kPhaseBandMs = 2.0;
constexpr int kPhaseWarnAfterSecs = 3;
int g_phase_danger_secs = 0;

// Sub-stage costs for the present currently in flight (present thread only).
std::array<double, N> g_pending{};

// 1-second accumulation window (present thread only).
struct Window {
    double interval = 0.0;
    double work = 0.0;
    double sleep = 0.0;
    double over = 0.0;
    double worst = 0.0; // longest single frame interval in the window (ms)
    double phase = -1.0; // latest vblank-interval phase sample (ms; <0 = no PLL)
    double target = 16.67; // latest per-flip pacing target (ms); the phase wraps at this
    std::array<double, N> stage{};
    int frames = 0;
    double elapsed_ms = 0.0; // summed interval; proxy for wall time in the window
    double clock_ms = 0.0;   // running clock for the leading timestamp
};
Window g_win{};

} // namespace

bool PresentLogEnabled() {
    return PresentFile() != nullptr;
}

PresentOnscreen PresentGetOnscreen() {
    return g_onscreen;
}

const PresentSeries& PresentGetSeries() {
    return g_series;
}

void PresentCountSubmit() {
    if (PresentFile() == nullptr) {
        return;
    }
    g_submit_count.fetch_add(1, std::memory_order_relaxed);
}

void PresentAddGpuWait(double ms) {
    if (PresentFile() == nullptr) {
        return;
    }
    g_gpuwait_us.fetch_add(static_cast<u64>(ms * 1000.0 + 0.5), std::memory_order_relaxed);
}

void PresentAddGpuBusy(double ms) {
    if (PresentFile() == nullptr) {
        return;
    }
    g_gpubusy_us.fetch_add(static_cast<u64>(ms * 1000.0 + 0.5), std::memory_order_relaxed);
}

void PresentMark(PresentStage stage, double ms) {
    if (PresentFile() == nullptr) {
        return;
    }
    g_pending[static_cast<int>(stage)] += ms;
}

void PresentFrame(double interval_ms, double work_ms, double sleep_ms, double target_ms,
                  double phase_ms) {
    std::FILE* file = PresentFile();
    if (file == nullptr) {
        return;
    }

    g_win.phase = phase_ms; // latest sample is representative; no need to average
    g_win.target = target_ms;
    g_win.interval += interval_ms;
    g_win.work += work_ms;
    g_win.sleep += sleep_ms;
    g_win.over += interval_ms - target_ms;
    if (interval_ms > g_win.worst) {
        g_win.worst = interval_ms;
    }
    for (int i = 0; i < N; ++i) {
        g_win.stage[i] += g_pending[i];
        g_pending[i] = 0.0;
    }
    g_win.frames += 1;
    g_win.elapsed_ms += interval_ms;
    g_win.clock_ms += interval_ms;

    if (g_win.elapsed_ms < 1000.0) {
        return;
    }

    const double n = static_cast<double>(g_win.frames);
    const double mean_interval = g_win.interval / n;
    const double fps = mean_interval > 0.0 ? 1000.0 / mean_interval : 0.0;
    const double window_s = g_win.elapsed_ms / 1000.0;
    // Diff the cross-thread counters over this window -> per-second rates.
    const u64 submit_cur = g_submit_count.load(std::memory_order_relaxed);
    const u64 gpuwait_cur = g_gpuwait_us.load(std::memory_order_relaxed);
    const u64 gpubusy_cur = g_gpubusy_us.load(std::memory_order_relaxed);
    const double submit_fps =
        window_s > 0.0 ? static_cast<double>(submit_cur - g_submit_last) / window_s : 0.0;
    const double gpuwait_ms =
        window_s > 0.0 ? static_cast<double>(gpuwait_cur - g_gpuwait_last) / 1000.0 / window_s : 0.0;
    const double gpubusy_ms =
        window_s > 0.0 ? static_cast<double>(gpubusy_cur - g_gpubusy_last) / 1000.0 / window_s : 0.0;
    g_submit_last = submit_cur;
    g_gpuwait_last = gpuwait_cur;
    g_gpubusy_last = gpubusy_cur;

    // Active phase-parking detection: a tick sitting within kPhaseBandMs of a
    // vblank boundary is one jitter-width away from display-side frame drops.
    // With the PLL steering toward mid-interval this should never fire; if it
    // does, the pacer is being overpowered and the episode must self-announce.
    const bool phase_danger =
        g_win.phase >= 0.0 &&
        (g_win.phase < kPhaseBandMs || g_win.phase > g_win.target - kPhaseBandMs);
    g_phase_danger_secs = phase_danger ? g_phase_danger_secs + 1 : 0;
    const bool phase_warn = g_phase_danger_secs >= kPhaseWarnAfterSecs;

    g_onscreen = PresentOnscreen{static_cast<float>(fps),       static_cast<float>(submit_fps),
                                 static_cast<float>(gpuwait_ms), static_cast<float>(gpubusy_ms),
                                 static_cast<float>(g_win.worst), static_cast<float>(g_win.phase),
                                 phase_warn,                     true};
    const int slot = g_series.count % PresentSeries::kN;
    g_series.fps[slot] = static_cast<float>(fps);
    g_series.submit[slot] = static_cast<float>(submit_fps);
    g_series.worst[slot] = static_cast<float>(g_win.worst);
    g_series.phase[slot] = static_cast<float>(g_win.phase);
    g_series.gpubusy[slot] = static_cast<float>(gpubusy_ms);
    g_series.gpuwait[slot] = static_cast<float>(gpuwait_ms);
    g_series.count += 1;
    std::fprintf(file,
                 "%.2f present n=%d fps=%.1f submit=%.1f worst=%.1f phase=%.2f gpuwait=%.1f "
                 "gpubusy=%.1f interval=%.2f work=%.2f acquire=%.2f record=%.2f flush=%.2f "
                 "qpresent=%.2f sleep=%.2f over=%.2f%s\n",
                 g_win.clock_ms / 1000.0, g_win.frames, fps, submit_fps, g_win.worst, g_win.phase,
                 gpuwait_ms, gpubusy_ms, mean_interval, g_win.work / n,
                 g_win.stage[static_cast<int>(PresentStage::Acquire)] / n,
                 g_win.stage[static_cast<int>(PresentStage::Record)] / n,
                 g_win.stage[static_cast<int>(PresentStage::Flush)] / n,
                 g_win.stage[static_cast<int>(PresentStage::QPresent)] / n, g_win.sleep / n,
                 g_win.over / n, phase_danger ? " PHASEWARN" : "");
    if (g_phase_danger_secs == kPhaseWarnAfterSecs || (phase_warn && g_phase_danger_secs % 30 == 0)) {
        std::fprintf(file,
                     "### phase-warning: present tick within %.1fms of the vblank boundary for %d "
                     "consecutive seconds (phase=%.2f of %.2f) -- display-side frame drops likely\n",
                     kPhaseBandMs, g_phase_danger_secs, g_win.phase, g_win.target);
    }
    std::fflush(file);

    const double keep_clock = g_win.clock_ms;
    g_win = Window{};
    g_win.clock_ms = keep_clock;
}

} // namespace Common

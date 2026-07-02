// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

namespace Common {

// Host present-thread pacing + cost telemetry. When SHAD_PRESENT_LOG names a
// file, emits one averaged summary line per second describing where each host
// present frame's wall time actually goes:
//   <t> present n=<frames> fps=<hz> interval=<ms> work=<ms> acquire=<ms>
//       record=<ms> flush=<ms> qpresent=<ms> sleep=<ms> over=<ms>
// interval = mean wall time between host flips; fps = 1000/interval = the HOST
//   present rate. worst = the SINGLE longest frame interval in the window (ms) --
//   so a sub-second burst of slow frames can't be hidden by the mean.
// submit   = guest flip-SUBMISSION rate (sceVideoOutSubmitFlip/sec): the game's
//   intended framerate before host pacing. If submit < fps the game is producing
//   frames slower than we present (upstream/GPU bound); if submit == fps == 60
//   the emulator is delivering 60 and any perceived drop is elsewhere.
// gpuwait  = total ms/sec the guest thread was blocked on the GPU (Scheduler::
//   Wait). Climbs when the GPU can't keep up -> guest throttles -> real slowdown.
// work     = CPU time spent inside Presenter::Present (acquire+record+flush+qp).
// sleep    = time the vblank pacer (AccurateTimer) actually slept this frame.
// over     = interval - vblank_target; the per-frame pacing overshoot. A steady
//   positive `over` with small `work` means we're pacing-bound (sleep drift); a
//   large `acquire` means we're GPU/swapchain-bound (backpressure). Most fields
//   are per-frame means over the 1-second window. No-op when the env is unset.
//
// Everything here runs on the single PresentThread, so no locking is required.
enum class PresentStage : int {
    Acquire,  // Swapchain::AcquireNextImage (blocks on a free swapchain image)
    Record,   // building the present command buffer (ImGui + blit)
    Flush,    // Scheduler::Flush (submit; may fold in a GPU wait)
    QPresent, // Swapchain::Present (vkQueuePresentKHR)
    Count,
};

// True when SHAD_PRESENT_LOG is set. Cheap; use to skip building sub-timings.
bool PresentLogEnabled();

// Last per-second summary values, for an on-screen readout that can be A/B'd
// against an external overlay. `valid` is false until the first second elapses.
struct PresentOnscreen {
    float fps;
    float submit;
    float gpuwait;
    float gpubusy; // GPU-busy ms per second (utilization; ~1000 = saturated)
    float worst;
    float phase;     // tick phase within the vblank interval (ms; <0 = no PLL)
    bool phase_warn; // true while the tick has sat near the vblank boundary
    bool valid;
};
PresentOnscreen PresentGetOnscreen();

// Count one guest flip SUBMISSION (sceVideoOutSubmitFlip). Called from the guest
// videoout thread -- the game's INTENDED framerate, before host pacing. Atomic.
void PresentCountSubmit();

// Add `ms` of CPU-blocked-on-GPU time (Scheduler::Wait). If this climbs while the
// present rate holds, the GPU is the bottleneck (guest throttled waiting on it).
// Called from the GPU-emulation thread. Atomic. No-op when logging is off.
void PresentAddGpuWait(double ms);

// Add `ms` of actual GPU-BUSY time, measured via Vulkan timestamp queries around
// the guest's command-buffer submissions. Summed per second this is the real GPU
// utilization: ~1000 ms/s => the GPU is saturated (render-bound); well under =>
// the GPU has headroom and any framerate loss is elsewhere (display/present).
// This is the one layer the present/submit counters can't see. Atomic.
void PresentAddGpuBusy(double ms);

// Add a sub-cost sample (ms) for the present currently in flight. Folded into
// the window on the next PresentFrame call. PresentThread only. No-op if unset.
void PresentMark(PresentStage stage, double ms);

// Call once per completed present-thread flip with the flip interval, the total
// CPU time spent in Present(), the pacer sleep for this iteration, the vblank
// target (ms), and the tick's phase within the display's vblank interval (ms;
// negative when phase locking is inactive -- logged as-is). A phase-locked
// pacer should hold `phase` steady near mid-interval (~8.3ms at 60Hz); a value
// wandering toward 0/16.7 means the tick is parked on the vblank boundary.
// Accumulates and emits an averaged line once per second.
void PresentFrame(double interval_ms, double work_ms, double sleep_ms, double target_ms,
                  double phase_ms = -1.0);

// RAII sub-cost timer for a PresentStage. ~free when logging is disabled.
struct PresentScope {
    PresentStage stage;
    bool active;
    std::chrono::steady_clock::time_point t0;
    explicit PresentScope(PresentStage s)
        : stage{s}, active{PresentLogEnabled()},
          t0{active ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}} {}
    ~PresentScope() {
        if (!active) {
            return;
        }
        PresentMark(stage, std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0)
                               .count());
    }
};

} // namespace Common

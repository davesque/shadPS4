// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include "common/profile_log.h"

namespace Common {

// Live stutter telemetry. When SHAD_STUTTER_LOG names a file, every displayed
// frame whose interval exceeds SHAD_STUTTER_MS (default 24ms) appends a
// self-contained breakdown of the per-category work done during that frame:
//   <t> stutter <frame_ms> translate=.. pipeline=.. tex=.. buf=.. imgcreate=.. wait=..
// so real stutters can be attributed during normal play, no reproduction needed.
// Categories sum to LESS than frame_ms when something un-instrumented is at play
// (the gap is the "unaccounted" remainder). No-op when the env var is unset.
enum class StutterCat : int {
    Translate, // GCN -> SPIR-V recompile
    Pipeline,  // vkCreateGraphicsPipelines / compute pipeline creation
    TexUpload, // TextureCache::RefreshImage (detile + upload + dirty hashing)
    BufUpload, // BufferCache::UploadCopies (geometry/vertex/index/uniform staging)
    ImgCreate, // Image ctor (vkCreateImage + device-memory allocation)
    GpuWait,   // Scheduler::Wait (CPU blocked on GPU)
    FileIo,    // guest file reads (ReadFile funnel: host I/O + cache invalidation)
    Count,
};

// Accumulate `ms` of work into the running per-category total. Cheap + atomic,
// safe from any thread. No-op when SHAD_STUTTER_LOG is unset.
void StutterAdd(StutterCat cat, double ms);

// Call once per displayed frame with the frame interval (ms). Always advances
// the per-category bookkeeping; emits a breakdown line only on a slow frame.
void StutterFrame(double frame_ms);

// RAII helper timing a scope into a stutter category. Also forwards to ProfileLog
// (under `name`) so the bench's SHAD_PROFILE timeline still works. Both sinks are
// env-gated internally, so this is ~free when neither is enabled.
struct StutterScope {
    StutterCat cat;
    const char* name;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    StutterScope(StutterCat cat_, const char* name_) : cat{cat_}, name{name_} {}
    ~StutterScope() {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        StutterAdd(cat, ms);
        ProfileLog(name, ms);
    }
};

} // namespace Common

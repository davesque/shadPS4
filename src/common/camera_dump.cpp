// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "common/camera_dump.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/types.h"

namespace Common {

namespace {

// --- Verified guest layout (ELF vaddrs; see camera_dump.h header) -------------
constexpr uintptr_t kContainerGlobal = 0x553e860; // *(base+this) = FD4 owner singleton
constexpr uintptr_t kContainerToMgr = 0x2830;     // owner[this] = camera manager
constexpr uintptr_t kManagerVtable = 0x539fe30;   // manager[0] must equal base+this
constexpr uintptr_t kMgrToFollowCam = 0x60;       // mgr[0xc] = ChrFollowCam
// ChrFollowCam field probes used to sanity-check / annotate.
constexpr uintptr_t kFovYOff = 0x50;         // f32 radians, expect 0.7505 (43 deg)
constexpr uintptr_t kEnableOff = 0x342;      // byte, EnableChrFollowCam
// Chase/rotation block to dump (inclusive, step 4).
constexpr uintptr_t kDumpBegin = 0x100;
constexpr uintptr_t kDumpEnd = 0x300;

// --- Player resolution (for movement/turn RE) --------------------------------
// WorldChrMan is a named FD4 singleton (the camera-manager step asserts the
// name "WorldChrMan" on the global at ELF 0x553e878). Its +0x60 slot is the
// character manager, which exposes the local player through a virtual
// GetLocalPlayer (manager_vtable+0x560/+0x598). We can't follow that virtual
// call statically, so dump the manager's vtable (to decompile the getter) and
// locate the player object at runtime by matching its world position to the
// camera pivot (followcam+0x90).
constexpr uintptr_t kWorldChrManGlobal = 0x553e878; // *(base+this) = WorldChrMan
constexpr uintptr_t kWcmToChrManager = 0x60;        // WorldChrMan[0xc] = ChrManager
constexpr uintptr_t kPivotOff = 0x90;               // followcam pivot (player pos proxy)
// How far a candidate object's position may sit from the pivot and still count
// as "the player", meters. The pivot chases the player at 0.1/tick so it trails
// by up to ~1 m under sprint; 3 m is a safe, still-discriminating window.
constexpr float kPosMatchM = 3.0f;
// Windows of WorldChrMan / manager to snapshot as raw u64 for offline layout
// analysis, and the span of a candidate object to scan for a position match.
constexpr uintptr_t kWcmWindow = 0x100;
constexpr uintptr_t kMgrWindow = 0x400;
constexpr uintptr_t kCandScan = 0x600;

// Poll cadence. First valid resolution dumps immediately; then we re-dump every
// kRedumpEverySecs so values that settle after boot (or are live-tuned) land in
// the log. Cheap (129 reads + fprintf) so the periodic re-dump is negligible.
constexpr int kPollEverySecs = 1;
constexpr int kRedumpEverySecs = 5;

std::atomic<bool> g_started{false};
// Manager slot the follow camera was found in (annotation only).
uintptr_t g_found_slot = 0;

// Read a POD value from a host address. shadPS4 maps guest memory 1:1 into the
// host process, and MemoryPatcher::g_eboot_address is already a directly
// dereferenceable host pointer (the memory patcher memcpy's through it), so a
// guest pointer stored in that memory is usable as a host pointer as-is.
// memcpy avoids alignment / strict-aliasing concerns.
template <typename T>
T Read(uintptr_t host_addr) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(host_addr), sizeof(T));
    return v;
}

// Access-violation-guarded read. The camera chain is validated link by link,
// but resolving the PLAYER means chasing pointers whose targets may not be
// committed (WorldChrMan's manager holds sparse slots). A wild deref would
// crash the game mid-session; SEH turns it into a clean false so the poller
// keeps running. POD-only body (no C++ unwinding inside __try).
bool SafeReadBytes(uintptr_t addr, void* out, size_t n) {
#ifdef _WIN32
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(out, reinterpret_cast<const void*>(addr), n);
    return true;
#endif
}

template <typename T>
bool SafeRead(uintptr_t addr, T& out) {
    return SafeReadBytes(addr, &out, sizeof(T));
}

// A value that looks like a live heap object pointer: guest heap sits below
// the eboot base (the camera resolved at guest 0x226c86450, base 0x800000000)
// and well above the low reserved range.
bool IsHeapPtr(uintptr_t v, uintptr_t base) {
    return v >= 0x100000000ull && v < base;
}

// Human-readable names for the individually-mapped offsets (from
// docs/MOVEMENT_CAMERA_SPEC_V2.md). Everything else in the block is emitted
// unlabeled for magnitude-matching against the spec's field list.
const char* FieldName(uintptr_t off) {
    switch (off) {
    case 0x130: return "ZoomInFovY";
    case 0x134: return "ZoomOutFovY";
    case 0x138: return "ZoomRate";
    case 0x13c: return "ZoomRateNormalMin";
    case 0x140: return "ZoomSpeed";
    case 0x15c: return "DirectPitchAng";
    case 0x160: return "DirectPitchRate";
    case 0x164: return "CamTurnBaseRotY";
    case 0x168: return "CamTurnRate";
    case 0x16c: return "CamTurnRateIncTime";
    case 0x1ec: return "RotRangeMaxX";
    case 0x1f0: return "RotRangeMinX";
    case 0x1f4: return "RotRangeAtLockMaxX";
    case 0x1f8: return "RotRangeAtLockMinX";
    case 0x1fc: return "Begin";
    case 0x200: return "End";
    default: return nullptr;
    }
}

std::FILE* OpenDumpFile() {
    const char* p = std::getenv("SHAD_CAMERA_DUMP");
    if (p == nullptr || *p == '\0') {
        return nullptr;
    }
    return std::fopen(p, "w");
}

// Resolve the live ChrFollowCam guest address, or 0 if not yet available /
// invalid. Only dereferences pointers gated by a preceding non-null check and,
// for the manager, a vtable match, so an uninitialized (zero) chain is skipped
// rather than followed into garbage.
uintptr_t ResolveFollowCam(uintptr_t base, uintptr_t& manager_out, uintptr_t& slot_out) {
    manager_out = 0;
    slot_out = 0;
    if (base == 0) {
        return 0;
    }
    const uintptr_t container = Read<uintptr_t>(base + kContainerGlobal);
    if (container < 0x10000) {
        return 0; // not constructed yet (0) or implausible
    }
    const uintptr_t manager = Read<uintptr_t>(container + kContainerToMgr);
    if (manager < 0x10000) {
        return 0;
    }
    // The live object's vtable turned out not to match the one the Init
    // disassembly predicted (a related class constructs here), so exact
    // equality was wrong. Require only that the vtable points into the
    // eboot image, then validate the sub-camera BY CONTENT below.
    const uintptr_t vtable = Read<uintptr_t>(manager);
    if (vtable < base || vtable > base + 0x10000000) {
        return 0;
    }
    manager_out = manager;
    // Probe the manager's pointer slots for the follow camera: the right
    // object carries FovY = 0.7505 rad (43 deg, LockCamParam-verified) at
    // +0x50. Prefer the predicted slot (+0x60), then scan neighbors.
    auto plausible = [&](uintptr_t cand) -> bool {
        if (cand < 0x10000) {
            return false;
        }
        const u32 raw = Read<u32>(cand + kFovYOff);
        float fovy;
        std::memcpy(&fovy, &raw, sizeof(fovy));
        return fovy > 0.5f && fovy < 1.2f;
    };
    const uintptr_t predicted = Read<uintptr_t>(manager + kMgrToFollowCam);
    if (plausible(predicted)) {
        slot_out = kMgrToFollowCam;
        return predicted;
    }
    for (uintptr_t off = 0x40; off < 0x100; off += 8) {
        const uintptr_t cand = Read<uintptr_t>(manager + off);
        if (plausible(cand)) {
            slot_out = off;
            return cand;
        }
    }
    return 0;
}

void EmitLine(std::FILE* file, const char* line) {
    if (file != nullptr) {
        std::fputs(line, file);
        std::fputc('\n', file);
        // Flush per line: the dump exists to be read while the game
        // is still running, and a buffered "waiting" line reads as a
        // dead poller from outside.
        std::fflush(file);
    }
    LOG_INFO(Loader, "{}", line);
}

// Read three consecutive floats as a position.
struct Vec3f {
    float x, y, z;
};

bool NearPivot(const Vec3f& p, const Vec3f& pivot) {
    auto ok = [](float a, float b) {
        const float d = a - b;
        return d > -kPosMatchM && d < kPosMatchM;
    };
    // Reject garbage: coordinates in a sane world range.
    auto sane = [](float a) { return a > -100000.0f && a < 100000.0f; };
    return sane(p.x) && sane(p.y) && sane(p.z) && ok(p.x, pivot.x) && ok(p.y, pivot.y) &&
           ok(p.z, pivot.z);
}

// Scan one candidate object's first kCandScan bytes for a float-triple that
// matches the pivot, and log the object + vtable + offset if found. Returns
// true on a match. All reads guarded, so a bad candidate is harmless.
bool ScanCandidate(std::FILE* file, uintptr_t base, uintptr_t cand, const Vec3f& pivot,
                   const char* origin) {
    if (!IsHeapPtr(cand, base)) {
        return false;
    }
    static thread_local unsigned char win[kCandScan];
    if (!SafeReadBytes(cand, win, sizeof(win))) {
        return false;
    }
    for (uintptr_t off = 0; off + sizeof(Vec3f) <= sizeof(win); off += 4) {
        Vec3f p;
        std::memcpy(&p, win + off, sizeof(p));
        if (NearPivot(p, pivot)) {
            uintptr_t vt = 0;
            std::memcpy(&vt, win, sizeof(vt));
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "CAMDUMP PLAYER? obj 0x%llx (from %s) pos@+0x%llx = (%.3f, %.3f, %.3f) "
                          "vtable 0x%llx (ELF 0x%llx)",
                          static_cast<unsigned long long>(cand), origin,
                          static_cast<unsigned long long>(off), p.x, p.y, p.z,
                          static_cast<unsigned long long>(vt),
                          static_cast<unsigned long long>(vt >= base ? vt - base : 0));
            EmitLine(file, buf);
            return true;
        }
    }
    return false;
}

// Resolve WorldChrMan + the character manager, dump the manager vtable (static
// RE anchor for GetLocalPlayer) and raw layout windows, and try to locate the
// player object by position-matching the camera pivot.
void DumpPlayer(std::FILE* file, uintptr_t base, uintptr_t followcam) {
    char buf[256];
    Vec3f pivot{};
    SafeReadBytes(followcam + kPivotOff, &pivot, sizeof(pivot));
    std::snprintf(buf, sizeof(buf), "CAMDUMP --- player probe: pivot (%.3f, %.3f, %.3f) ---",
                  pivot.x, pivot.y, pivot.z);
    EmitLine(file, buf);

    uintptr_t wcm = 0;
    if (!SafeRead(base + kWorldChrManGlobal, wcm) || !IsHeapPtr(wcm, base)) {
        EmitLine(file, "CAMDUMP player: WorldChrMan not resolved yet");
        return;
    }
    uintptr_t mgr = 0;
    SafeRead(wcm + kWcmToChrManager, mgr);
    uintptr_t mgr_vt = 0;
    if (IsHeapPtr(mgr, base)) {
        SafeRead(mgr, mgr_vt);
    }
    std::snprintf(buf, sizeof(buf),
                  "CAMDUMP WorldChrMan 0x%llx  ChrManager(+0x60) 0x%llx  mgr vtable 0x%llx "
                  "(ELF 0x%llx)",
                  static_cast<unsigned long long>(wcm), static_cast<unsigned long long>(mgr),
                  static_cast<unsigned long long>(mgr_vt),
                  static_cast<unsigned long long>(mgr_vt >= base ? mgr_vt - base : 0));
    EmitLine(file, buf);

    // Raw layout windows (u64) for offline analysis of where the player hangs.
    for (uintptr_t off = 0; off < kWcmWindow; off += 8) {
        uintptr_t v = 0;
        if (SafeRead(wcm + off, v) && v != 0) {
            std::snprintf(buf, sizeof(buf), "CAMDUMP   WCM +0x%03llx = 0x%llx%s",
                          static_cast<unsigned long long>(off), static_cast<unsigned long long>(v),
                          IsHeapPtr(v, base) ? "  [heap]" : "");
            EmitLine(file, buf);
        }
    }
    // Position-match scan over WorldChrMan and the manager's pointer slots.
    bool found = false;
    for (uintptr_t off = 0; off < kWcmWindow && !found; off += 8) {
        uintptr_t v = 0;
        if (SafeRead(wcm + off, v)) {
            found = ScanCandidate(file, base, v, pivot, "WCM slot");
        }
    }
    if (IsHeapPtr(mgr, base)) {
        for (uintptr_t off = 0; off < kMgrWindow; off += 8) {
            uintptr_t v = 0;
            if (SafeRead(mgr + off, v)) {
                std::snprintf(buf, sizeof(buf), "CAMDUMP   MGR +0x%03llx = 0x%llx%s",
                              static_cast<unsigned long long>(off),
                              static_cast<unsigned long long>(v),
                              IsHeapPtr(v, base) ? "  [heap]" : "");
                if (v != 0) {
                    EmitLine(file, buf);
                }
                ScanCandidate(file, base, v, pivot, "MGR slot");
            }
        }
    }
}

void DumpOnce(std::FILE* file, uintptr_t base, uintptr_t manager, uintptr_t followcam) {
    char buf[256];

    const uintptr_t fovy_i = Read<u32>(followcam + kFovYOff);
    float fovy;
    std::memcpy(&fovy, &fovy_i, sizeof(fovy));
    const u8 enable = Read<u8>(followcam + kEnableOff);

    // The object's vtable pointer is the key that unlocks static RE of
    // its Update: vtable - eboot_base = the ELF data address whose
    // slots name every virtual method.
    const uintptr_t cam_vtable = Read<uintptr_t>(followcam);
    const uintptr_t mgr_vtable = Read<uintptr_t>(manager);
    std::snprintf(buf, sizeof(buf),
                  "CAMDUMP === ChrFollowCam @ guest 0x%llx vtable 0x%llx (manager 0x%llx "
                  "vtable 0x%llx slot +0x%llx, eboot base 0x%llx) FovY=%.5f rad "
                  "EnableChrFollowCam=%u ===",
                  static_cast<unsigned long long>(followcam),
                  static_cast<unsigned long long>(cam_vtable),
                  static_cast<unsigned long long>(manager),
                  static_cast<unsigned long long>(mgr_vtable),
                  static_cast<unsigned long long>(g_found_slot),
                  static_cast<unsigned long long>(base), fovy, enable);
    EmitLine(file, buf);

    for (uintptr_t off = kDumpBegin; off <= kDumpEnd; off += 4) {
        const u32 raw = Read<u32>(followcam + off);
        float f;
        std::memcpy(&f, &raw, sizeof(f));
        const char* name = FieldName(off);
        if (name != nullptr) {
            std::snprintf(buf, sizeof(buf), "CAMDUMP +0x%03llx = 0x%08x (f32 = %.6g)  [%s]",
                          static_cast<unsigned long long>(off), raw, f, name);
        } else {
            std::snprintf(buf, sizeof(buf), "CAMDUMP +0x%03llx = 0x%08x (f32 = %.6g)",
                          static_cast<unsigned long long>(off), raw, f);
        }
        EmitLine(file, buf);
    }
    DumpPlayer(file, base, followcam);
    if (file != nullptr) {
        std::fflush(file);
    }
}

void PollLoop(std::FILE* file) {
    const uintptr_t base = MemoryPatcher::g_eboot_address;

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "CAMDUMP poller started. eboot base = 0x%llx. Manual-fallback guest "
                      "addresses (for a memory viewer): container=*(0x%llx), "
                      "manager=*(container+0x%llx), followcam=*(manager+0x%llx); dump "
                      "followcam+0x%llx..+0x%llx.",
                      static_cast<unsigned long long>(base),
                      static_cast<unsigned long long>(base + kContainerGlobal),
                      static_cast<unsigned long long>(kContainerToMgr),
                      static_cast<unsigned long long>(kMgrToFollowCam),
                      static_cast<unsigned long long>(kDumpBegin),
                      static_cast<unsigned long long>(kDumpEnd));
        EmitLine(file, buf);
    }

    int since_dump = kRedumpEverySecs; // force first dump as soon as resolved
    int waited = 0;
    while (true) {
        uintptr_t manager = 0;
        uintptr_t slot = 0;
        const uintptr_t followcam = ResolveFollowCam(base, manager, slot);
        g_found_slot = slot;
        if (followcam != 0) {
            since_dump += kPollEverySecs;
            if (since_dump >= kRedumpEverySecs) {
                DumpOnce(file, base, manager, followcam);
                since_dump = 0;
            }
        } else {
            // Not resolved: every 10 s show each raw link of the chain
            // so a failure names the link that broke (container not
            // yet constructed, manager slot empty, vtable mismatch).
            if (waited % 10 == 0) {
                char buf[256];
                const uintptr_t container = Read<uintptr_t>(base + kContainerGlobal);
                const uintptr_t mgr =
                    container >= 0x10000 ? Read<uintptr_t>(container + kContainerToMgr) : 0;
                const uintptr_t vtbl = mgr >= 0x10000 ? Read<uintptr_t>(mgr) : 0;
                std::snprintf(buf, sizeof(buf),
                              "CAMDUMP waiting: container=0x%llx manager=0x%llx vtable=0x%llx "
                              "(expect 0x%llx)",
                              static_cast<unsigned long long>(container),
                              static_cast<unsigned long long>(mgr),
                              static_cast<unsigned long long>(vtbl),
                              static_cast<unsigned long long>(base + kManagerVtable));
                EmitLine(file, buf);
            }
            waited += kPollEverySecs;
        }
        std::this_thread::sleep_for(std::chrono::seconds(kPollEverySecs));
    }
}

} // namespace

void StartCameraDump() {
    std::FILE* file = OpenDumpFile();
    if (file == nullptr) {
        return; // SHAD_CAMERA_DUMP unset -> complete no-op
    }
    if (g_started.exchange(true)) {
        std::fclose(file);
        return; // already running
    }
    LOG_INFO(Loader, "SHAD_CAMERA_DUMP set: starting Bloodborne camera-struct dumper");
    std::thread(PollLoop, file).detach();
}

} // namespace Common

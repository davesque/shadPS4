// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
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
// ChrManager+0x60 is the local player ChrIns (runtime-confirmed: that slot held
// the vtable-0x5370430 object whose destructors write that vtable). Its world
// transform is a 4x4 at +0x180 (right +0x180, up +0x190, forward +0x1a0,
// translation +0x1b0), so the forward vector at +0x1a0 is the character's
// facing. Dump +0x140..+0x220 each snapshot to pin the facing/velocity fields.
constexpr uintptr_t kMgrToPlayer = 0x60;
constexpr uintptr_t kPlayerVtableElf = 0x5370430;
constexpr uintptr_t kXformBegin = 0x140;
constexpr uintptr_t kXformEnd = 0x220;
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

// --- Character-facing hunt (follow-up capture) -------------------------------
// The camera step caches the CAMERA yaw at player+0x54/+0x17c every frame, so
// those are the camera-relative reference frame, NOT the character's own facing.
// The true facing lives in some other field. Sample a wide player-field window
// at a high rate to a CSV so a controlled two-phase maneuver (turn the character
// with the camera held still, then pan the camera with the character held still)
// reveals offline which field tracks the character vs the camera.
// Round 1 found the facing: player+0x34 = the character's true world-facing angle
// (radians). Round 2 narrows the window (0x34 + 0x54 both fit under 0x200) and
// samples FAST so the +0x34 staircase reveals the game-LOGIC frame period (step
// ~16.7 ms => 60 fps, ~33.3 ms => 30 fps) and the peak turn slew is not aliased.
constexpr uintptr_t kFieldWindow = 0x200; // bytes of the player object to sample
constexpr int kFieldHz = 150;             // sampler rate (oversample vs 60 fps logic)
// Arm the write-watchpoint, now pointed at the true facing field (see
// InstallHeadingWatch). On for round 2 to catch the +0x34 writer = the real
// turn/steering code.
constexpr bool kArmHeadingWatch = true;

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

// Probe whether [addr, addr+n) is entirely committed and readable WITHOUT
// touching it. This is the real guard. shadPS4 installs a vectored exception
// handler for guest memory (signals.cpp) that intercepts access violations
// before any frame-based SEH __except can run -- confirmed by a crash reading
// guest 0x500000000 that was reported as "Unhandled Exception" despite the
// __try below. So a wild deref cannot be caught after it faults; it must be
// avoided. The player probe chases pointers whose targets may be
// reserved-but-uncommitted (the manager holds sparse slots and stale/garbage
// values), so every speculative read is gated on this first.
bool IsReadable(uintptr_t addr, size_t n) {
#ifdef _WIN32
    uintptr_t cur = addr;
    const uintptr_t end = addr + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            return false; // reserved / free -> reading faults
        }
        constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kReadable) == 0 ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        cur = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
#else
    (void)addr;
    (void)n;
    return true;
#endif
}

// Guarded read: never fault. IsReadable rejects unmapped / guarded ranges up
// front (the only reliable protection given shadPS4's vectored handler); the
// __try is a harmless second line for a race between the probe and the copy.
// POD-only body (no C++ unwinding inside __try).
bool SafeReadBytes(uintptr_t addr, void* out, size_t n) {
#ifdef _WIN32
    if (!IsReadable(addr, n)) {
        return false;
    }
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

// Labels for the hypothesized player world-transform 4x4 (column-major) so the
// dump is readable at a glance; unlabeled offsets print raw for magnitude match.
const char* XformTag(uintptr_t off) {
    switch (off) {
    case 0x180: return "right.x";
    case 0x190: return "up.x";
    case 0x1a0: return "fwd.x (facing)";
    case 0x1a4: return "fwd.y";
    case 0x1a8: return "fwd.z (facing)";
    case 0x1b0: return "pos.x";
    case 0x1b4: return "pos.y";
    case 0x1b8: return "pos.z";
    default: return nullptr;
    }
}

// Where the dump lands when SHAD_CAMERA_DUMP is unset: a file next to the
// running executable. This makes the capture work from a launcher that can't
// set environment variables (BB Launcher) with zero per-machine config, while
// the env var still wins when present so output can be redirected.
// A path to `name` in the directory of the running executable (so output lands
// next to shadPS4.exe under a launcher that can't set env vars), falling back to
// the working directory if the module path can't be resolved.
std::string SiblingPath(const char* name) {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string path(buf, n);
        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos) {
            path.resize(slash + 1);
            path += name;
            return path;
        }
    }
#endif
    return name; // fallback: current working directory
}

std::string DefaultDumpPath() {
    return SiblingPath("bb_camera_dump.txt");
}

std::FILE* OpenDumpFile(std::string& path_out) {
    const char* p = std::getenv("SHAD_CAMERA_DUMP");
    if (p != nullptr && *p != '\0') {
        path_out = p;
    } else {
        path_out = DefaultDumpPath();
    }
    return std::fopen(path_out.c_str(), "w");
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
    uintptr_t container = 0;
    if (!SafeRead(base + kContainerGlobal, container) || container < 0x10000) {
        return 0; // not constructed yet (0) or implausible
    }
    uintptr_t manager = 0;
    if (!SafeRead(container + kContainerToMgr, manager) || manager < 0x10000) {
        return 0;
    }
    // The live object's vtable turned out not to match the one the Init
    // disassembly predicted (a related class constructs here), so exact
    // equality was wrong. Require only that the vtable points into the
    // eboot image, then validate the sub-camera BY CONTENT below.
    uintptr_t vtable = 0;
    if (!SafeRead(manager, vtable) || vtable < base || vtable > base + 0x10000000) {
        return 0;
    }
    manager_out = manager;
    // Probe the manager's pointer slots for the follow camera: the right
    // object carries FovY = 0.7505 rad (43 deg, LockCamParam-verified) at
    // +0x50. Prefer the predicted slot (+0x60), then scan neighbors. Each slot
    // value is an unvalidated pointer, so read through the guard.
    auto plausible = [&](uintptr_t cand) -> bool {
        if (cand < 0x10000) {
            return false;
        }
        u32 raw = 0;
        if (!SafeRead(cand + kFovYOff, raw)) {
            return false;
        }
        float fovy;
        std::memcpy(&fovy, &raw, sizeof(fovy));
        return fovy > 0.5f && fovy < 1.2f;
    };
    uintptr_t predicted = 0;
    SafeRead(manager + kMgrToFollowCam, predicted);
    if (plausible(predicted)) {
        slot_out = kMgrToFollowCam;
        return predicted;
    }
    for (uintptr_t off = 0x40; off < 0x100; off += 8) {
        uintptr_t cand = 0;
        SafeRead(manager + off, cand);
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

// --- Facing-writer watchpoint (pure-static RE enabler) -----------------------
// The character's true world-facing angle is player+0x34 (round-1 CSV capture:
// it sweeps as the character turns and is inert to camera panning, unlike the
// camera yaw at +0x54/+0x17c). Its writer -- the turn/steering code -- is invoked
// through the engine's callback dispatch, so static call-graph tracing can't
// reach it, and the offset is overloaded across dozens of classes so a field-scan
// only finds false positives. A hardware DATA WRITE watchpoint pinpoints it: set
// DR0 on player+0x34 (and DR1 on player+0x54 as a reference, a KNOWN camera-yaw
// writer) on every thread; a write raises #DB and a vectored handler logs the
// faulting guest RIP. RIP - eboot_base = the ELF vaddr of the exact writing
// instruction = the function to decompile. PS4 code runs as native host x86-64,
// so the guest write is a real host instruction and the debug registers trap it.
#ifdef _WIN32
std::atomic<bool> g_watch_installed{false};
uintptr_t g_watch_base = 0;
HANDLE g_watch_log = INVALID_HANDLE_VALUE;
std::atomic_flag g_watch_lock = ATOMIC_FLAG_INIT;
constexpr int kMaxWatchRips = 64;
uintptr_t g_watch_seen[kMaxWatchRips];
int g_watch_seen_n = 0;

void WatchWrite(const char* s, int n) {
    if (g_watch_log != INVALID_HANDLE_VALUE && n > 0) {
        DWORD wrote = 0;
        WriteFile(g_watch_log, s, static_cast<DWORD>(n), &wrote, nullptr);
    }
}

// Vectored handler: on a hardware data #DB, log the distinct writing RIP.
// Minimal + POD only (runs in exception context on arbitrary threads).
LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    const DWORD64 dr6 = ctx->Dr6;
    if ((dr6 & 0xf) == 0) {
        return EXCEPTION_CONTINUE_SEARCH; // not one of our data breakpoints
    }
    const int which = (dr6 & 0x1) ? 0 : (dr6 & 0x2) ? 1 : (dr6 & 0x4) ? 2 : 3;
    const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
    const uintptr_t elf = (rip >= g_watch_base) ? rip - g_watch_base : rip;
    const uintptr_t key = (static_cast<uintptr_t>(which) << 60) ^ elf;
    bool is_new = false;
    while (g_watch_lock.test_and_set(std::memory_order_acquire)) {
    }
    bool seen = false;
    for (int i = 0; i < g_watch_seen_n; ++i) {
        if (g_watch_seen[i] == key) {
            seen = true;
            break;
        }
    }
    if (!seen && g_watch_seen_n < kMaxWatchRips) {
        g_watch_seen[g_watch_seen_n++] = key;
        is_new = true;
    }
    g_watch_lock.clear(std::memory_order_release);
    if (is_new) {
        char buf[192];
        const int n = std::snprintf(
            buf, sizeof(buf), "WATCH dr%d (%s) writer ELF 0x%llx  (rip 0x%llx)\n", which,
            which == 0 ? "player+0x34 facing (true)" : which == 1 ? "player+0x54 camera-yaw ref" : "?",
            static_cast<unsigned long long>(elf), static_cast<unsigned long long>(rip));
        WatchWrite(buf, n);
    }
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Set DR0=a0 / DR1=a1 as 4-byte WRITE watchpoints on every thread but this one.
void SetHwWatchAllThreads(uintptr_t a0, uintptr_t a1) {
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
                continue;
            }
            HANDLE th = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                te.th32ThreadID);
            if (th == nullptr) {
                continue;
            }
            SuspendThread(th);
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = a0;
                ctx.Dr1 = a1;
                // L0+L1 enable; RW=01 (write), LEN=11 (4 bytes) for slots 0 and 1.
                DWORD64 dr7 = ctx.Dr7 & ~0x00ff000full;
                dr7 |= (1ull << 0) | (1ull << 2);
                dr7 |= (0b01ull << 16) | (0b11ull << 18);
                dr7 |= (0b01ull << 20) | (0b11ull << 22);
                ctx.Dr7 = dr7;
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(th, &ctx);
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void InstallHeadingWatch(std::FILE* file, uintptr_t base, uintptr_t player) {
    if (g_watch_installed.exchange(true)) {
        return;
    }
    g_watch_base = base;
    char path[MAX_PATH];
    const DWORD pn = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = (pn > 0 && pn < MAX_PATH) ? std::string(path, pn) : std::string();
    const size_t slash = p.find_last_of("\\/");
    p = (slash != std::string::npos) ? p.substr(0, slash + 1) : std::string();
    p += "heading_watch.txt";
    g_watch_log = CreateFileA(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    WatchWrite("# heading_watch: distinct RIPs of instructions that WRITE the watched fields\n", 76);
    AddVectoredExceptionHandler(1, WatchVeh);
    // DR0 = the true facing field found in round 1 (its writer is the steering
    // code we want); DR1 = camera yaw, kept as a reference so the run re-confirms
    // the known camera writer and cleanly separates the two.
    const uintptr_t a34 = player + 0x34;
    const uintptr_t a54 = player + 0x54;
    SetHwWatchAllThreads(a34, a54);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "CAMDUMP heading watch armed: DR0=player+0x34 (0x%llx), DR1=player+0x54 "
                  "(0x%llx); writer RIPs -> %s",
                  static_cast<unsigned long long>(a34), static_cast<unsigned long long>(a54),
                  p.c_str());
    EmitLine(file, buf);
}
#else
void InstallHeadingWatch(std::FILE*, uintptr_t, uintptr_t) {}
#endif

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

    // Focused local-player transform dump: the authoritative facing/position
    // source, read straight from ChrManager+0x60 (no position-match guessing).
    // Across snapshots at different headings, the facing floats rotate while
    // position tracks the walk, which pins the turn behavior for the RE.
    uintptr_t player = 0;
    SafeRead(mgr + kMgrToPlayer, player);
    if (IsHeapPtr(player, base)) {
        uintptr_t pvt = 0;
        SafeRead(player, pvt);
        const bool match = (pvt == base + kPlayerVtableElf);
        std::snprintf(buf, sizeof(buf),
                      "CAMDUMP PLAYER obj 0x%llx vtable 0x%llx (ELF 0x%llx) %s",
                      static_cast<unsigned long long>(player),
                      static_cast<unsigned long long>(pvt),
                      static_cast<unsigned long long>(pvt >= base ? pvt - base : 0),
                      match ? "[vtable MATCH]" : "[vtable mismatch]");
        EmitLine(file, buf);
        // Arm the hardware write-watchpoint on the confirmed player once, to
        // catch the dispatched turn code that writes the target heading.
        if (match && kArmHeadingWatch) {
            InstallHeadingWatch(file, base, player);
        }
        for (uintptr_t off = kXformBegin; off < kXformEnd; off += 4) {
            u32 raw = 0;
            if (!SafeRead(player + off, raw)) {
                continue;
            }
            float f;
            std::memcpy(&f, &raw, sizeof(f));
            const char* tag = XformTag(off);
            if (tag != nullptr) {
                std::snprintf(buf, sizeof(buf), "CAMDUMP   PLR +0x%03llx = %11.5f  [%s]",
                              static_cast<unsigned long long>(off), f, tag);
            } else {
                std::snprintf(buf, sizeof(buf), "CAMDUMP   PLR +0x%03llx = %11.5f",
                              static_cast<unsigned long long>(off), f);
            }
            EmitLine(file, buf);
        }
    }

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

// Resolve the local player ChrIns via WorldChrMan -> ChrManager(+0x60) -> +0x60,
// returning it only if its vtable matches the known player class so that load
// screens / uninitialized slots (garbage or stale pointers) are skipped. 0 if
// not currently available. Every hop is guard-read so a wild pointer never
// faults (shadPS4's vectored handler would treat that as fatal).
uintptr_t ResolvePlayerMatched(uintptr_t base) {
    uintptr_t wcm = 0;
    if (!SafeRead(base + kWorldChrManGlobal, wcm) || !IsHeapPtr(wcm, base)) {
        return 0;
    }
    uintptr_t mgr = 0;
    if (!SafeRead(wcm + kWcmToChrManager, mgr) || !IsHeapPtr(mgr, base)) {
        return 0;
    }
    uintptr_t player = 0;
    if (!SafeRead(mgr + kMgrToPlayer, player) || !IsHeapPtr(player, base)) {
        return 0;
    }
    uintptr_t pvt = 0;
    if (!SafeRead(player, pvt) || pvt != base + kPlayerVtableElf) {
        return 0;
    }
    return player;
}

// High-rate wide-window sampler: log player+0x00..+kFieldWindow as f32 columns
// to player_fields.csv at kFieldHz, for the whole session. Offline, the column
// that sweeps a full angular range during the "turn the character, camera held
// still" phase but stays flat during the "pan the camera, character held still"
// phase is the character's true facing (the camera yaw at +0x54/+0x17c does the
// opposite). Separate file + thread so it never perturbs the readable dump; the
// row is flushed each sample so a mid-capture crash keeps everything so far.
void PlayerFieldLoop() {
    const std::string path = SiblingPath("player_fields.csv");
    std::FILE* csv = std::fopen(path.c_str(), "w");
    if (csv == nullptr) {
        LOG_WARNING(Loader, "Bloodborne player-field sampler: could not open '{}'", path);
        return;
    }
    LOG_INFO(Loader, "Bloodborne player-field sampler active -> {}", path);

    std::fprintf(csv, "t_ms");
    for (uintptr_t off = 0; off < kFieldWindow; off += 4) {
        std::fprintf(csv, ",0x%03llx", static_cast<unsigned long long>(off));
    }
    std::fprintf(csv, "\n");
    std::fflush(csv);

    const auto t0 = std::chrono::steady_clock::now();
    const auto period = std::chrono::milliseconds(1000 / kFieldHz);
    u8 window[kFieldWindow];
    while (true) {
        const uintptr_t base = MemoryPatcher::g_eboot_address;
        const uintptr_t player = base != 0 ? ResolvePlayerMatched(base) : 0;
        if (player != 0 && SafeReadBytes(player, window, kFieldWindow)) {
            const long long t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count();
            std::fprintf(csv, "%lld", t_ms);
            for (uintptr_t off = 0; off < kFieldWindow; off += 4) {
                float f;
                std::memcpy(&f, window + off, sizeof(f));
                std::fprintf(csv, ",%.6g", f);
            }
            std::fprintf(csv, "\n");
            std::fflush(csv);
        }
        std::this_thread::sleep_for(period);
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
                uintptr_t container = 0;
                SafeRead(base + kContainerGlobal, container);
                uintptr_t mgr = 0;
                if (container >= 0x10000) {
                    SafeRead(container + kContainerToMgr, mgr);
                }
                uintptr_t vtbl = 0;
                if (mgr >= 0x10000) {
                    SafeRead(mgr, vtbl);
                }
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
    // Claim the singleton first: opening with "w" truncates, so a re-entrant
    // call must not reopen the file the running poller already holds.
    if (g_started.exchange(true)) {
        return; // already running
    }
    std::string path;
    std::FILE* file = OpenDumpFile(path);
    if (file == nullptr) {
        LOG_WARNING(Loader, "Bloodborne camera dumper: could not open '{}'", path);
        g_started.store(false); // let a later game-load retry
        return;
    }
    LOG_INFO(Loader, "Bloodborne camera+player dumper active -> {}", path);
    std::thread(PollLoop, file).detach();
    // Independent high-rate sampler for the character-facing hunt (own file).
    std::thread(PlayerFieldLoop).detach();
}

} // namespace Common

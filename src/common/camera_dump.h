// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {

// Dev-only Bloodborne (CUSA03173, 1.09) camera-config dumper.
//
// Bloodborne builds its ~90 ChrFollowCam "feel" constants at runtime (via an FD4
// factory + bulk memcpy, then reads them through a vtable call), so they are
// invisible to static decompilation but trivially readable once the struct is
// populated in guest RAM. This poller resolves the live ChrFollowCam object out
// of emulated guest memory and dumps the +0x100..+0x300 chase/rotation block to
// a log so the values can be matched to the field names in
// docs/MOVEMENT_CAMERA_SPEC_V2.md.
//
// Activation: set the environment variable SHAD_CAMERA_DUMP to an output file
// path before launching. When unset, this is a complete no-op. Output is written
// to that file (one CAMDUMP line per field) and mirrored to LOG_INFO(Loader).
//
// Guest-address resolution (all ELF vaddrs, verified from the decrypted 1.09
// eboot; guest/host addr = MemoryPatcher::g_eboot_address + ELF_vaddr because
// shadPS4 maps the eboot 1:1 into host address space):
//   container = *(base + 0x553e860)      // FD4 scene/camera-owner singleton
//   manager   = *(container + 0x2830)    // FD4 camera manager (validated by vtable)
//   followcam = *(manager + 0x60)        // mgr[0xc] = ChrFollowCam (dump target)
// Validation: manager[0] must equal (base + 0x539fe30), the manager's vtable.
//
// Safe to call once after the eboot module is loaded (g_eboot_address set). It
// spawns a detached background thread that polls until the manager appears, then
// re-dumps periodically so late-populated / live-tuned values are captured.
void StartCameraDump();

} // namespace Common

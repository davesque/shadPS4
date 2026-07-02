// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Input {

class GameControllers;

// Scripted controller input for benchmark / closed-loop mode.
//
// When SHAD_BENCH is set and SHAD_INPUT_SCRIPT points to a script file, this
// drives the virtual controller on slot 0 through a timed sequence of
// button/stick commands, so the harness can reach and exercise *in-game*
// scenarios (e.g. load a save, then walk/fight) with no human at the pad. This
// is what makes the closed loop able to profile real in-game stutter instead of
// only the boot-to-title workload.
//
// No-op outside benchmark mode or when SHAD_INPUT_SCRIPT is unset, so normal
// boot-to-title benchmarking is unchanged.
void StartBenchInput(GameControllers* controllers);

// Input RECORDING (the mirror of the above): when SHAD_INPUT_RECORD names a file,
// real controller state changes are appended in the same script grammar, so a
// real play session can be captured and later replayed via SHAD_INPUT_SCRIPT for
// a realistic, continuous in-game profiling workload. No-op when the env is unset,
// and independent of benchmark mode (records during normal play). Args are passed
// as primitives to keep this header free of input/pad type dependencies; the
// implementation maps them back to button/axis names.
void RecordButton(u32 button_bit, bool pressed); // button_bit = OrbisPadButtonDataOffset value
void RecordAxis(u32 axis_index, int value);      // axis_index = Input::Axis underlying value

} // namespace Input

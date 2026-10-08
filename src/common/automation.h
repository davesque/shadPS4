// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

namespace Common {
inline bool IsAutomationMode() {
    static const bool enabled = std::getenv("SHAD_AUTOMATION_USER_DIR") != nullptr;
    return enabled;
}
} // namespace Common

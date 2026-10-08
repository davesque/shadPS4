// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"
#include "input/controller_tuning.h"

namespace Input {

using Libraries::Pad::OrbisPadButtonDataOffset;

void State::OnButton(OrbisPadButtonDataOffset button, bool is_pressed) {
    if (is_pressed) {
        buttonsState |= button;
    } else {
        buttonsState &= ~button;
    }
}

void State::OnAxis(Axis axis, int value, u64 timestamp, bool smooth) {
    const auto index = std::to_underlying(axis);
    axes[index] = axis_smoothing_end_values[index];

    axis_smoothing_start_times[index] = timestamp;
    axis_smoothing_start_values[index] = axes[index];
    axis_smoothing_end_values[index] = value;
    axis_smoothing_flags[index] = smooth;
    const auto toggle = [&](const auto button) {
        if (value > 0) {
            buttonsState |= button;
        } else {
            buttonsState &= ~button;
        }
    };
    switch (axis) {
    case Axis::TriggerLeft:
        toggle(OrbisPadButtonDataOffset::L2);
        break;
    case Axis::TriggerRight:
        toggle(OrbisPadButtonDataOffset::R2);
        break;
    default:
        break;
    }
}

void State::OnTouchpad(int touch_index, bool is_down, float x, float y) {
    touchpad[touch_index].state = is_down;
    touchpad[touch_index].x = static_cast<u16>(x * 1920);
    touchpad[touch_index].y = static_cast<u16>(y * 941);
}

void State::OnGyro(const float gyro[3]) {
    angularVelocity.x = gyro[0];
    angularVelocity.y = gyro[1];
    angularVelocity.z = gyro[2];
}

void State::OnAccel(const float accel[3]) {
    acceleration.x = accel[0];
    acceleration.y = accel[1];
    acceleration.z = accel[2];
}

void State::UpdateAxisSmoothing(u64 timestamp) {
    static const u64 smoothing_time = []() -> u64 {
        const char* value = std::getenv("SHAD_AXIS_SMOOTH_MS");
        if (value && *value) {
            const auto duration = ParseAxisSmoothing(value);
            if (duration) {
                LOG_INFO(Input, "Axis smoothing configured to {} microseconds", duration->count());
                return static_cast<u64>(duration->count());
            }
            LOG_WARNING(Input, "Invalid SHAD_AXIS_SMOOTH_MS '{}'; disabling smoothing", value);
        }
        return 0;
    }();
    for (int i = 0; i < std::to_underlying(Axis::AxisMax); ++i) {
        if (smoothing_time == 0 || !axis_smoothing_flags[i] ||
            std::abs(axes[i] - axis_smoothing_end_values[i]) < 16) {
            axes[i] = axis_smoothing_end_values[i];
            continue;
        }
        const f32 t = std::clamp((timestamp - axis_smoothing_start_times[i]) /
                                     static_cast<f32>(smoothing_time),
                                 0.f, 1.f);
        axes[i] = s32(axis_smoothing_start_values[i] * (1 - t) + axis_smoothing_end_values[i] * t);
    }
}

} // namespace Input

// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <string_view>

namespace Input {

struct CameraResponse {
    float sensitivity = 1.0f;
    float curve = 1.0f;

    std::int16_t Apply(std::int16_t value) const;
};

class CameraTuning {
public:
    using Clock = std::chrono::steady_clock;
    using Reporter = std::function<void(std::string_view)>;

    explicit CameraTuning(std::filesystem::path path = {}, Reporter reporter = {});
    std::int16_t Apply(std::int16_t value, Clock::time_point now = Clock::now());
    void Read(std::istream& stream);
    const CameraResponse& Response() const;

private:
    std::filesystem::path path;
    Reporter reporter;
    CameraResponse response;
    std::optional<Clock::time_point> last_read;
};

std::optional<std::chrono::microseconds> ParseAxisSmoothing(std::string_view milliseconds);

} // namespace Input

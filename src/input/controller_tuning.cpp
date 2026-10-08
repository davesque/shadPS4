// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/controller_tuning.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <utility>

namespace Input {

namespace {

std::string_view Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

template <typename T>
std::optional<T> ParseNumber(std::string_view value) {
    value = Trim(value);
    if (value.empty()) {
        return std::nullopt;
    }
    T result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

} // namespace

std::int16_t CameraResponse::Apply(std::int16_t value) const {
    if (sensitivity == 1.0f && curve == 1.0f) {
        return value;
    }
    const float magnitude = std::min(std::abs(static_cast<int>(value)) / 128.0f, 1.0f);
    const float shaped = std::min(std::pow(magnitude, curve) * sensitivity, 1.0f);
    return static_cast<std::int16_t>((value < 0 ? -1.0f : 1.0f) * shaped * 128.0f);
}

CameraTuning::CameraTuning(std::filesystem::path path_, Reporter reporter_)
    : path{std::move(path_)}, reporter{std::move(reporter_)} {}

std::int16_t CameraTuning::Apply(std::int16_t value, Clock::time_point now) {
    if (!path.empty() && (!last_read || now - *last_read >= std::chrono::milliseconds{500})) {
        last_read = now;
        std::ifstream stream{path};
        if (stream) {
            Read(stream);
        }
    }
    return response.Apply(value);
}

void CameraTuning::Read(std::istream& stream) {
    const auto previous = response;
    std::string line;
    while (std::getline(stream, line)) {
        const auto text = Trim(line);
        if (text.empty() || text.starts_with('#')) {
            continue;
        }
        const auto separator = text.find('=');
        if (separator == std::string_view::npos) {
            if (reporter) {
                reporter("Ignoring camera tuning line without '='");
            }
            continue;
        }
        const auto key = Trim(text.substr(0, separator));
        const auto value = ParseNumber<float>(text.substr(separator + 1));
        if (key != "sens" && key != "curve") {
            if (reporter) {
                reporter("Ignoring unknown camera tuning key");
            }
        } else if (!value || !std::isfinite(*value) ||
                   (key == "sens" ? *value <= 0.0f : *value < 0.05f)) {
            if (reporter) {
                reporter("Ignoring invalid camera tuning value");
            }
        } else if (key == "sens") {
            response.sensitivity = *value;
        } else {
            response.curve = *value;
        }
    }
    if (reporter &&
        (previous.sensitivity != response.sensitivity || previous.curve != response.curve)) {
        reporter("Camera tuning updated: sens=" + std::to_string(response.sensitivity) +
                 ", curve=" + std::to_string(response.curve));
    }
}

const CameraResponse& CameraTuning::Response() const {
    return response;
}

std::optional<std::chrono::microseconds> ParseAxisSmoothing(std::string_view milliseconds) {
    const auto value = ParseNumber<std::int64_t>(milliseconds);
    if (!value || *value < 0 || *value > std::numeric_limits<std::int64_t>::max() / 1000) {
        return std::nullopt;
    }
    return std::chrono::microseconds{*value * 1000};
}

} // namespace Input

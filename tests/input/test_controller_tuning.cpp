// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "input/controller_tuning.h"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestResponse() {
    const Input::CameraResponse linear{1.0f, 1.0f};
    const Input::CameraResponse tuned{1.55f, 1.1f};
    const Input::CameraResponse defaults;
    Check(defaults.sensitivity == 1.0f && defaults.curve == 1.0f,
          "Default response must be neutral");
    int previous = 0;
    for (int value = 0; value <= 128; ++value) {
        Check(linear.Apply(value) == value, "Linear response must preserve input");
        Check(linear.Apply(-value) == -value, "Linear response must preserve negative input");
        Check(defaults.Apply(value) == value, "Default response must preserve input");
        const auto output = tuned.Apply(value);
        Check(output >= previous && output <= 128, "Tuned response must be bounded and monotonic");
        Check(tuned.Apply(-value) == -output, "Tuned response must be symmetric");
        previous = output;
    }
    Check(tuned.Apply(0) == 0, "Neutral stick must remain neutral");
    Check(tuned.Apply(128) == 128, "Full stick must reach maximum output");
    Check(tuned.Apply(64) > 64, "Higher sensitivity must increase mid-stick response");
    Check(Input::CameraResponse{1.0f, 2.0f}.Apply(64) == 32, "Curve must shape intermediate input");
    Check(Input::CameraResponse{0.5f, 1.0f}.Apply(128) == 64,
          "Sensitivity must allow slower response");
}

void TestParsing() {
    std::vector<std::string> messages;
    Input::CameraTuning tuning{{},
                               [&](std::string_view message) { messages.emplace_back(message); }};
    std::istringstream valid{"# settings\n sens = 2.0 \n curve = 0.5\n\n"};
    tuning.Read(valid);
    Check(tuning.Response().sensitivity == 2.0f && tuning.Response().curve == 0.5f,
          "Whitespace and comments must be accepted");
    Check(!messages.empty(), "Updates must be observable");
    for (const auto* invalid : {"nan", "inf", "-inf", "0", "-1", "1junk", "", "1e99"}) {
        std::istringstream stream{std::string{"sens="} + invalid};
        tuning.Read(stream);
        Check(tuning.Response().sensitivity == 2.0f,
              "Invalid sensitivity must preserve previous value");
    }
    std::istringstream malformed{"curve=0.01\ncurve=nan\nmissing separator\nother=3\n"};
    tuning.Read(malformed);
    Check(tuning.Response().curve == 0.5f, "Invalid curve must preserve previous value");
    std::istringstream partial{"sens=1\n"};
    tuning.Read(partial);
    Check(tuning.Response().sensitivity == 1.0f && tuning.Response().curve == 0.5f,
          "Partial files must preserve unspecified settings");
    std::istringstream lower_bound{"curve=0.05\n"};
    tuning.Read(lower_bound);
    Check(tuning.Response().curve == 0.05f, "Minimum supported curve must be accepted");
}

void TestSmoothingParsing() {
    Check(Input::ParseAxisSmoothing("0")->count() == 0, "Zero must disable smoothing");
    Check(Input::ParseAxisSmoothing(" 33 ")->count() == 33000,
          "Milliseconds must convert to microseconds");
    Check(Input::ParseAxisSmoothing("10")->count() == 10000, "Custom windows must be supported");
    for (const auto* invalid :
         {"", "-1", "1.5", "abc", "33junk", "9223372036854776", "9223372036854775808"}) {
        Check(!Input::ParseAxisSmoothing(invalid),
              "Invalid and overflowing windows must be rejected");
    }
}

class TuneFile {
public:
    TuneFile()
        : path{std::filesystem::temp_directory_path() /
               ("shadps4-controller-test-" +
                std::to_string(Input::CameraTuning::Clock::now().time_since_epoch().count()) +
                ".txt")} {}
    ~TuneFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    void Write(const std::string& text) const {
        std::ofstream stream{path};
        stream << text;
        Check(stream.good(), "Fixture file must be writable");
    }
    std::filesystem::path path;
};

void TestLiveReload() {
    using namespace std::chrono_literals;
    TuneFile file;
    Input::CameraTuning tuning{file.path};
    const auto start = Input::CameraTuning::Clock::time_point{};
    file.Write("sens=1\ncurve=1\n");
    Check(tuning.Apply(64, start) == 64, "First use must read the file");
    file.Write("sens=2\n");
    Check(tuning.Apply(64, start + 499ms) == 64, "File reads must be throttled");
    Check(tuning.Apply(64, start + 500ms) == 128, "Changes must reload after 500 milliseconds");
    std::filesystem::remove(file.path);
    Check(tuning.Apply(64, start + 1000ms) == 128, "Missing file must retain settings");
    file.Write("sens=0.5\n");
    Check(tuning.Apply(64, start + 1500ms) == 32, "Recreated file must resume live tuning");
}

} // namespace

int main() {
    try {
        TestResponse();
        TestParsing();
        TestSmoothingParsing();
        TestLiveReload();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

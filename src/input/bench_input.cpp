// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/bench.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/libraries/pad/pad.h"
#include "input/bench_input.h"
#include "input/controller.h"

namespace Input {

using Libraries::Pad::OrbisPadButtonDataOffset;

namespace {

// A single timed action in the input script. Times are seconds from when the
// driver thread starts (which is right after InitTimers, i.e. ~game start).
struct BenchCommand {
    double time_s{};
    enum class Kind { Button, Axis, Log } kind{};
    OrbisPadButtonDataOffset button{}; // Kind::Button
    bool pressed{};                    // Kind::Button
    Input::Axis axis{};                // Kind::Axis
    int axis_value{};                  // Kind::Axis (0..255, 128 = centered)
    std::string message;               // Kind::Log
};

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::optional<OrbisPadButtonDataOffset> ParseButton(std::string_view s) {
    using B = OrbisPadButtonDataOffset;
    if (s == "cross" || s == "x")
        return B::Cross;
    if (s == "circle" || s == "o")
        return B::Circle;
    if (s == "triangle")
        return B::Triangle;
    if (s == "square")
        return B::Square;
    if (s == "up")
        return B::Up;
    if (s == "down")
        return B::Down;
    if (s == "left")
        return B::Left;
    if (s == "right")
        return B::Right;
    if (s == "l1")
        return B::L1;
    if (s == "r1")
        return B::R1;
    if (s == "l2")
        return B::L2;
    if (s == "r2")
        return B::R2;
    if (s == "l3")
        return B::L3;
    if (s == "r3")
        return B::R3;
    if (s == "options" || s == "start")
        return B::Options;
    if (s == "touchpad")
        return B::TouchPad;
    return std::nullopt;
}

std::optional<Input::Axis> ParseAxis(std::string_view s) {
    if (s == "leftx")
        return Input::Axis::LeftX;
    if (s == "lefty")
        return Input::Axis::LeftY;
    if (s == "rightx")
        return Input::Axis::RightX;
    if (s == "righty")
        return Input::Axis::RightY;
    if (s == "l2")
        return Input::Axis::TriggerLeft;
    if (s == "r2")
        return Input::Axis::TriggerRight;
    return std::nullopt;
}

// Reverse maps (enum -> script name) for recording.
const char* ButtonName(u32 b) {
    using B = OrbisPadButtonDataOffset;
    switch (static_cast<B>(b)) {
    case B::Cross:
        return "cross";
    case B::Circle:
        return "circle";
    case B::Triangle:
        return "triangle";
    case B::Square:
        return "square";
    case B::Up:
        return "up";
    case B::Down:
        return "down";
    case B::Left:
        return "left";
    case B::Right:
        return "right";
    case B::L1:
        return "l1";
    case B::R1:
        return "r1";
    case B::L2:
        return "l2";
    case B::R2:
        return "r2";
    case B::L3:
        return "l3";
    case B::R3:
        return "r3";
    case B::Options:
        return "options";
    case B::TouchPad:
        return "touchpad";
    default:
        return nullptr;
    }
}

const char* AxisName(u32 a) {
    switch (static_cast<Input::Axis>(a)) {
    case Input::Axis::LeftX:
        return "leftx";
    case Input::Axis::LeftY:
        return "lefty";
    case Input::Axis::RightX:
        return "rightx";
    case Input::Axis::RightY:
        return "righty";
    case Input::Axis::TriggerLeft:
        return "l2";
    case Input::Axis::TriggerRight:
        return "r2";
    default:
        return nullptr;
    }
}

// Append one timestamped line (seconds since the first recorded event) to the
// SHAD_INPUT_RECORD file, in the same grammar StartBenchInput replays.
void RecordEvent(const std::string& rest) {
    static std::FILE* file = []() -> std::FILE* {
        const char* p = std::getenv("SHAD_INPUT_RECORD");
        return (p != nullptr && *p != '\0') ? std::fopen(p, "w") : nullptr;
    }();
    if (file == nullptr) {
        return;
    }
    static const auto start = std::chrono::steady_clock::now();
    static std::mutex mtx;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::scoped_lock lk{mtx};
    std::fprintf(file, "%.3f %s\n", t, rest.c_str());
    std::fflush(file);
}

void AddButton(std::vector<BenchCommand>& cmds, double t, OrbisPadButtonDataOffset b, bool pressed) {
    BenchCommand c;
    c.time_s = t;
    c.kind = BenchCommand::Kind::Button;
    c.button = b;
    c.pressed = pressed;
    cmds.push_back(std::move(c));
}

void AddAxis(std::vector<BenchCommand>& cmds, double t, Input::Axis a, int v) {
    BenchCommand c;
    c.time_s = t;
    c.kind = BenchCommand::Kind::Axis;
    c.axis = a;
    c.axis_value = std::clamp(v, 0, 255);
    cmds.push_back(std::move(c));
}

// Script grammar (one command per line, '#' starts a comment):
//   <t> press|release <button>
//   <t> tap|hold <button> [duration_s=0.1]   ; press at t, release at t+duration
//   <t> axis <leftx|lefty|rightx|righty|l2|r2> <0..255>
//   <t> stick <left|right> <x 0..255> <y 0..255>   ; 128,128 = centered
//   <t> recenter                                   ; both sticks to 128,128
//   <t> log <message...>
std::vector<BenchCommand> ParseScript(const std::string& path) {
    std::vector<BenchCommand> cmds;
    std::ifstream f(path);
    if (!f) {
        LOG_ERROR(Input, "Bench input: could not open script '{}'", path);
        return cmds;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (const auto h = line.find('#'); h != std::string::npos) {
            line.erase(h);
        }
        std::istringstream ss(line);
        double t;
        if (!(ss >> t)) {
            continue; // blank or comment-only line
        }
        std::string verb;
        if (!(ss >> verb)) {
            continue;
        }
        verb = ToLower(verb);
        if (verb == "press" || verb == "release") {
            std::string b;
            ss >> b;
            if (const auto btn = ParseButton(ToLower(b))) {
                AddButton(cmds, t, *btn, verb == "press");
            } else {
                LOG_ERROR(Input, "Bench input: unknown button '{}' (line {})", b, lineno);
            }
        } else if (verb == "tap" || verb == "hold") {
            std::string b;
            double dur = 0.1;
            ss >> b >> dur;
            if (const auto btn = ParseButton(ToLower(b))) {
                AddButton(cmds, t, *btn, true);
                AddButton(cmds, t + dur, *btn, false);
            } else {
                LOG_ERROR(Input, "Bench input: unknown button '{}' (line {})", b, lineno);
            }
        } else if (verb == "axis") {
            std::string a;
            int v = 128;
            ss >> a >> v;
            if (const auto ax = ParseAxis(ToLower(a))) {
                AddAxis(cmds, t, *ax, v);
            } else {
                LOG_ERROR(Input, "Bench input: unknown axis '{}' (line {})", a, lineno);
            }
        } else if (verb == "stick") {
            std::string side;
            int x = 128, y = 128;
            ss >> side >> x >> y;
            side = ToLower(side);
            if (side == "left") {
                AddAxis(cmds, t, Input::Axis::LeftX, x);
                AddAxis(cmds, t, Input::Axis::LeftY, y);
            } else if (side == "right") {
                AddAxis(cmds, t, Input::Axis::RightX, x);
                AddAxis(cmds, t, Input::Axis::RightY, y);
            } else {
                LOG_ERROR(Input, "Bench input: unknown stick side '{}' (line {})", side, lineno);
            }
        } else if (verb == "recenter") {
            AddAxis(cmds, t, Input::Axis::LeftX, 128);
            AddAxis(cmds, t, Input::Axis::LeftY, 128);
            AddAxis(cmds, t, Input::Axis::RightX, 128);
            AddAxis(cmds, t, Input::Axis::RightY, 128);
        } else if (verb == "log") {
            BenchCommand c;
            c.time_s = t;
            c.kind = BenchCommand::Kind::Log;
            std::string rest;
            std::getline(ss, rest);
            if (!rest.empty() && rest.front() == ' ') {
                rest.erase(0, 1);
            }
            c.message = std::move(rest);
            cmds.push_back(std::move(c));
        } else {
            LOG_ERROR(Input, "Bench input: unknown verb '{}' (line {})", verb, lineno);
        }
    }
    std::stable_sort(cmds.begin(), cmds.end(),
                     [](const BenchCommand& a, const BenchCommand& b) { return a.time_s < b.time_s; });
    LOG_INFO(Input, "Bench input: loaded {} commands from '{}'", cmds.size(), path);
    return cmds;
}

std::jthread g_bench_input_thread;

} // namespace

void StartBenchInput(GameControllers* controllers) {
    if (!Common::IsBenchmarkMode()) {
        return;
    }
    const char* script_path = std::getenv("SHAD_INPUT_SCRIPT");
    if (script_path == nullptr || *script_path == '\0') {
        LOG_INFO(Input, "Bench input: SHAD_INPUT_SCRIPT not set; boot-to-title workload only");
        return;
    }
    auto commands = ParseScript(script_path);
    if (commands.empty()) {
        return;
    }
    GameController* controller = (*controllers)[0];

    g_bench_input_thread =
        std::jthread([commands = std::move(commands), controller](std::stop_token st) {
            Common::SetCurrentThreadName("shadPS4:BenchInput");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            const auto start = std::chrono::steady_clock::now();
            for (const auto& cmd : commands) {
                const auto due =
                    start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(cmd.time_s));
                // Sleep until the command is due, in short slices so a stop
                // request (shutdown) is noticed promptly.
                while (!st.stop_requested()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= due) {
                        break;
                    }
                    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
                        due - now, std::chrono::milliseconds(15)));
                }
                if (st.stop_requested()) {
                    return;
                }
                switch (cmd.kind) {
                case BenchCommand::Kind::Button:
                    controller->Button(cmd.button, cmd.pressed);
                    break;
                case BenchCommand::Kind::Axis:
                    controller->Axis(cmd.axis, cmd.axis_value, true);
                    break;
                case BenchCommand::Kind::Log:
                    LOG_INFO(Input, "Bench input @ {:.2f}s: {}", cmd.time_s, cmd.message);
                    break;
                }
            }
            LOG_INFO(Input, "Bench input: script complete");
        });
}

void RecordButton(u32 button_bit, bool pressed) {
    const char* name = ButtonName(button_bit);
    if (name == nullptr) {
        return;
    }
    RecordEvent(std::string(pressed ? "press " : "release ") + name);
}

void RecordAxis(u32 axis_index, int value) {
    const char* name = AxisName(axis_index);
    if (name == nullptr) {
        return;
    }
    RecordEvent("axis " + std::string(name) + " " + std::to_string(value));
}

} // namespace Input

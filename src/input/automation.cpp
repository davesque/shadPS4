// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/automation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <SDL3/SDL_events.h>

#include "common/automation.h"
#include "common/logging/log.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "video_core/renderdoc.h"

namespace Input {
namespace {
using Button = Libraries::Pad::OrbisPadButtonDataOffset;

struct Command {
    double time;
    std::string verb;
    std::string target;
    int value{};
};

class Automation {
public:
    Automation() {
        const char* path = std::getenv("SHAD_AUTOMATION_SCRIPT");
        if (!path) {
            throw std::runtime_error("Automation requires SHAD_AUTOMATION_SCRIPT");
        }
        std::ifstream stream{path};
        if (!stream) {
            throw std::runtime_error("Cannot open automation script");
        }
        std::string line;
        while (std::getline(stream, line)) {
            line = line.substr(0, line.find('#'));
            if (line.find_first_not_of(" \t\r") == std::string::npos) {
                continue;
            }
            Command cmd{};
            std::istringstream parser{line};
            if (!(parser >> cmd.time >> cmd.verb) || !std::isfinite(cmd.time) || cmd.time < 0) {
                throw std::runtime_error("Invalid automation timestamp");
            }
            if (cmd.verb == "press" || cmd.verb == "release") {
                if (!(parser >> cmd.target) || !buttons.contains(cmd.target)) {
                    throw std::runtime_error("Invalid automation button");
                }
            } else if (cmd.verb == "contact_left" || cmd.verb == "contact_right") {
                if (!(parser >> cmd.target) || (cmd.target != "down" && cmd.target != "up")) {
                    throw std::runtime_error("Invalid automation contact");
                }
            } else if (cmd.verb == "key_press" || cmd.verb == "key_release" ||
                       cmd.verb == "mapped_button_press" || cmd.verb == "mapped_button_release") {
                const bool gamepad = cmd.verb.starts_with("mapped_button");
                if (!(parser >> cmd.target) ||
                    (gamepad ? cmd.target != "back" : cmd.target != "g" && cmd.target != "h")) {
                    throw std::runtime_error("Invalid automation mapped input");
                }
            } else if (cmd.verb == "axis" || cmd.verb == "mapped_axis") {
                if (!(parser >> cmd.target >> cmd.value) || !axes.contains(cmd.target) ||
                    (cmd.verb == "axis" ? cmd.value < 0 || cmd.value > 255
                                        : cmd.value < -32768 || cmd.value > 32767)) {
                    throw std::runtime_error("Invalid automation axis");
                }
            } else if (cmd.verb != "screenshot" && cmd.verb != "recenter" && cmd.verb != "quit") {
                throw std::runtime_error("Invalid automation verb");
            }
            std::string extra;
            if (parser >> extra) {
                throw std::runtime_error("Unexpected automation arguments");
            }
            commands.push_back(std::move(cmd));
        }
        std::stable_sort(commands.begin(), commands.end(),
                         [](const Command& a, const Command& b) { return a.time < b.time; });
        LOG_INFO(Input, "Automation loaded {} commands", commands.size());
    }

    void Poll(GameControllers& controllers) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        while (next < commands.size() && commands[next].time <= elapsed) {
            const auto& cmd = commands[next++];
            auto* controller = controllers[0];
            if (cmd.verb == "press" || cmd.verb == "release") {
                controller->Button(buttons.at(cmd.target), cmd.verb == "press");
            } else if (cmd.verb == "contact_left" || cmd.verb == "contact_right") {
                controller->SetTouchpadState(0, cmd.target == "down",
                                             cmd.verb == "contact_left" ? 0.25f : 0.75f, 0.5f);
            } else if (cmd.verb == "key_press" || cmd.verb == "key_release" ||
                       cmd.verb == "mapped_button_press" || cmd.verb == "mapped_button_release") {
                const bool gamepad = cmd.verb.starts_with("mapped_button");
                const auto key =
                    gamepad ? SDL_GAMEPAD_BUTTON_BACK : (cmd.target == "g" ? SDLK_G : SDLK_H);
                const auto event =
                    InputEvent{gamepad ? InputType::Controller : InputType::KeyboardMouse, key,
                               cmd.verb.ends_with("press"), 0};
                if (UpdatePressedKeys(event)) {
                    ActivateOutputsFromInputs();
                }
            } else if (cmd.verb == "mapped_axis") {
                const auto event =
                    InputEvent{InputID{InputType::Axis, static_cast<u32>(axes.at(cmd.target)), 1},
                               true, static_cast<s8>(cmd.value / 256)};
                if (UpdatePressedKeys(event)) {
                    ActivateOutputsFromInputs();
                }
            } else if (cmd.verb == "axis") {
                controller->Axis(axes.at(cmd.target), cmd.value, false);
            } else if (cmd.verb == "recenter") {
                for (auto axis : {Axis::LeftX, Axis::LeftY, Axis::RightX, Axis::RightY}) {
                    controller->Axis(axis, 128, false);
                }
            } else if (cmd.verb == "quit") {
                SDL_Event event{};
                event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&event);
            } else {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
            LOG_INFO(Input, "Automation t={:.3f}: {} {} {}", elapsed, cmd.verb, cmd.target,
                     cmd.value);
        }
        if (elapsed >= next_screenshot) {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            next_screenshot = elapsed + 5.0;
        }
        if (next == commands.size() && !completed) {
            completed = true;
            LOG_INFO(Input, "Automation script complete");
            Common::Log::Flush();
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    const std::unordered_map<std::string, Button> buttons{{"cross", Button::Cross},
                                                          {"circle", Button::Circle},
                                                          {"triangle", Button::Triangle},
                                                          {"square", Button::Square},
                                                          {"options", Button::Options},
                                                          {"up", Button::Up},
                                                          {"down", Button::Down},
                                                          {"left", Button::Left},
                                                          {"right", Button::Right},
                                                          {"l1", Button::L1},
                                                          {"r1", Button::R1},
                                                          {"l2", Button::L2},
                                                          {"r2", Button::R2},
                                                          {"l3", Button::L3},
                                                          {"r3", Button::R3},
                                                          {"touchpad", Button::TouchPad}};
    const std::unordered_map<std::string, Axis> axes{
        {"leftx", Axis::LeftX},   {"lefty", Axis::LeftY},    {"rightx", Axis::RightX},
        {"righty", Axis::RightY}, {"l2", Axis::TriggerLeft}, {"r2", Axis::TriggerRight}};
    std::vector<Command> commands;
    Clock::time_point started = Clock::now();
    size_t next{};
    double next_screenshot{};
    bool completed{};
};
} // namespace

void PollAutomation(GameControllers& controllers) {
    static Automation automation;
    automation.Poll(controllers);
}
} // namespace Input

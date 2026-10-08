# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(ControllerTuningTests LANGUAGES CXX)
    enable_testing()
endif()

get_filename_component(SHADPS4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(shadps4_controller_tuning_test
    test_controller_tuning.cpp
    ${SHADPS4_ROOT}/src/input/controller_tuning.cpp
)
target_include_directories(shadps4_controller_tuning_test PRIVATE ${SHADPS4_ROOT}/src)
target_compile_features(shadps4_controller_tuning_test PRIVATE cxx_std_23)
add_test(NAME controller_tuning COMMAND shadps4_controller_tuning_test)

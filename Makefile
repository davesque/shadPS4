# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

CMAKE ?= cmake
BUILD_DIR ?= Build/controller-tuning-tests
FORMAT ?= clang-format
TUNING_SOURCES = src/input/controller_tuning.h src/input/controller_tuning.cpp src/input/controller_state.cpp src/input/controller.h src/input/input_handler.cpp tests/input/test_controller_tuning.cpp

.PHONY: test-controller-tuning
test-controller-tuning:
	$(CMAKE) -S tests/input -B $(BUILD_DIR)
	$(CMAKE) --build $(BUILD_DIR) --config Debug
	ctest --test-dir $(BUILD_DIR) -C Debug --output-on-failure

.PHONY: format-controller-tuning
format-controller-tuning:
	$(FORMAT) -i $(TUNING_SOURCES)

.PHONY: lint-controller-tuning
lint-controller-tuning:
	$(FORMAT) --dry-run --Werror $(TUNING_SOURCES)
	git diff --check

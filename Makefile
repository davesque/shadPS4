# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

CMAKE ?= cmake

.PHONY: build
build:
ifeq ($(OS),Windows_NT)
	cmd /c scripts\build-windows.cmd
else
	$(CMAKE) --build build_clang
endif

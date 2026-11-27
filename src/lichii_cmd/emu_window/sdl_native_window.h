// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/frontend/emu_window.h"

struct SDL_Window;

/// Fills `info` with the native handle SDL made for `window`, so the Vulkan backend can create
/// its surface: an HWND, wayland display/surface, X11 display/window, CAMetalLayer or Android
/// window. Returns false when SDL exposes none of them.
[[nodiscard]] bool SdlFillWindowInfo(SDL_Window* window,
                                     Core::Frontend::EmuWindow::WindowSystemInfo& info);
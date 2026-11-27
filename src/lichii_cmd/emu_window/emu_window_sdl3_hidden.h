// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "common/common_types.h"
#include "core/frontend/emu_window.h"

namespace Core::Frontend {
class GraphicsContext;
}

/// Makes the window a GPU trace replay presents on when the Vulkan driver has no
/// VK_EXT_headless_surface: an SDL window created hidden, which is never shown, focused or
/// interacted with. Returns null when SDL cannot make one (no video driver, for instance).
///
/// The window must outlive the renderer's Vulkan surface. The replay keeps it until it has shut
/// the GPU down, which destroys the renderer first.
[[nodiscard]] std::shared_ptr<Core::Frontend::EmuWindow> MakeHiddenReplayWindow(u32 width,
                                                                                u32 height);
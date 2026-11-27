// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "lichii_cmd/emu_window/emu_window_sdl3_vk.h"
#include "lichii_cmd/emu_window/sdl_native_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

EmuWindow_SDL3_VK::EmuWindow_SDL3_VK(InputCommon::InputSubsystem* input_subsystem_,
                                     Core::System& system_, bool fullscreen)
    : EmuWindow_SDL3{input_subsystem_, system_} {
    const std::string window_title = fmt::format("Eden {} | {}-{} (Vulkan)",
                                                 Common::g_build_name,
                                                 Common::g_scm_branch,
                                                 Common::g_scm_desc);
    render_window =
        SDL_CreateWindow(window_title.c_str(), Layout::ScreenUndocked::Width,
                         Layout::ScreenUndocked::Height,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | Nx2HiddenWindowFlags());

    if (render_window == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to create SDL3 window! {}", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }

    SetWindowIcon();

    if (fullscreen) {
        Fullscreen();
        ShowCursor(false);
    }

    if (!SdlFillWindowInfo(render_window, window_info)) {
        LOG_CRITICAL(Frontend, "Unable to determine native window backend from SDL properties");
        std::exit(EXIT_FAILURE);
    }

    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);
    SDL_PumpEvents();
    OnResize();
    LOG_INFO(Frontend, "Eden Version: {} | {}-{} (Vulkan)", Common::g_build_name,
             Common::g_scm_branch, Common::g_scm_desc);
}

EmuWindow_SDL3_VK::~EmuWindow_SDL3_VK() = default;

std::unique_ptr<Core::Frontend::GraphicsContext> EmuWindow_SDL3_VK::CreateSharedContext() const {
    return std::make_unique<DummyContext>();
}

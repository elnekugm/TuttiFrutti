// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii_cmd/emu_window/emu_window_sdl3_hidden.h"

#include <SDL3/SDL.h>

#include "common/logging.h"
#include "core/frontend/graphics_context.h"
#include "lichii_cmd/emu_window/sdl_native_window.h"

namespace {

class HiddenContext final : public Core::Frontend::GraphicsContext {};

/// The window a GPU trace replay presents on where the driver has no headless surface. It is
/// created hidden and never shown, but reports itself as shown so the renderer composites and
/// captures its frames; nothing of it reaches the desktop.
class HiddenWindow final : public Core::Frontend::EmuWindow {
public:
    HiddenWindow(SDL_Window* window_, u32 width, u32 height) : window{window_} {
        UpdateCurrentFramebufferLayout(width, height);
    }

    ~HiddenWindow() override {
        SDL_DestroyWindow(window);
        // Balanced by the constructor's SDL_InitSubSystem, even when SDL had video running.
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<HiddenContext>();
    }

    bool IsShown() const override {
        return true;
    }

    /// The window-system info still to be filled from SDL before the GPU starts.
    Core::Frontend::EmuWindow::WindowSystemInfo& SetupWindowInfo() {
        return window_info;
    }

private:
    SDL_Window* window{};
};

} // Anonymous namespace

std::shared_ptr<Core::Frontend::EmuWindow> MakeHiddenReplayWindow(u32 width, u32 height) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        LOG_ERROR(Frontend, "GPU trace replay: SDL video failed to start: {}", SDL_GetError());
        return nullptr;
    }
    SDL_Window* const window =
        SDL_CreateWindow("TuttiFrutti GPU trace replay", static_cast<int>(width),
                         static_cast<int>(height), SDL_WINDOW_HIDDEN | SDL_WINDOW_NOT_FOCUSABLE);
    if (window == nullptr) {
        LOG_ERROR(Frontend, "GPU trace replay: SDL could not make a hidden window: {}",
                  SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return nullptr;
    }

    auto emu_window = std::make_shared<HiddenWindow>(window, width, height);
    if (!SdlFillWindowInfo(window, emu_window->SetupWindowInfo())) {
        LOG_ERROR(Frontend, "GPU trace replay: SDL's hidden window has no native handle");
        return nullptr;
    }
    return emu_window;
}
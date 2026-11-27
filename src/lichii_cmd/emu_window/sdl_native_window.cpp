// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>

#include <SDL3/SDL.h>

#include "core/frontend/emu_window.h"
#include "lichii_cmd/emu_window/sdl_native_window.h"

bool SdlFillWindowInfo(SDL_Window* window, Core::Frontend::EmuWindow::WindowSystemInfo& info) {
    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);

    if (void* hwnd =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        info.type = Core::Frontend::WindowSystemType::Windows;
        info.render_surface = hwnd;
        return true;
    }
    if (void* wl_display = SDL_GetPointerProperty(
            window_props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
        void* wl_surface =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        if (wl_surface == nullptr) {
            return false;
        }
        info.type = Core::Frontend::WindowSystemType::Wayland;
        info.display_connection = wl_display;
        info.render_surface = wl_surface;
        return true;
    }
    if (void* x11_display =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)) {
        const auto x11_window =
            SDL_GetNumberProperty(window_props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (x11_window == 0) {
            return false;
        }
        info.type = Core::Frontend::WindowSystemType::X11;
        info.display_connection = x11_display;
        info.render_surface = reinterpret_cast<void*>(static_cast<uintptr_t>(x11_window));
        return true;
    }
    if (SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr) !=
        nullptr) {
        info.type = Core::Frontend::WindowSystemType::Cocoa;
        info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
        return true;
    }
    if (void* android_window =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)) {
        info.type = Core::Frontend::WindowSystemType::Android;
        info.render_surface = android_window;
        return true;
    }
    return false;
}
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Tools {

class RenderdocAPI {
public:
    explicit RenderdocAPI();
    ~RenderdocAPI();

    void ToggleCapture();

    /// EDEN_NX2_RENDERDOC_FRAME brackets one frame: BeginCapture from the end of the frame before
    /// it, EndCapture at its end. A headless run (the trace replay) has no hotkey to press.
    void BeginCapture();
    void EndCapture();

    /// Whether renderdoc.dll was found in this process (the run must be launched by renderdoccmd).
    [[nodiscard]] bool Available() const;

private:
    /// Looks renderdoc.dll up again. The Vulkan capture layer loads the DLL when the device is
    /// created, which is after this object is constructed.
    void Load();

    void* rdoc_api{};
    bool is_capturing{false};
};

} // namespace Tools

// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCommon {

/// The temporal upscaler that ran last: what the performance overlay shows. The renderer sets it
/// each time one evaluates, and clears it when it shuts down.
enum class TemporalUpscaler : u8 { None, Fsr31, Fsr4, Dlss };

inline std::atomic<TemporalUpscaler> active_temporal_upscaler{TemporalUpscaler::None};

inline void ReportTemporalUpscaler(TemporalUpscaler upscaler) {
    active_temporal_upscaler.store(upscaler, std::memory_order_relaxed);
}

[[nodiscard]] inline TemporalUpscaler ActiveTemporalUpscaler() {
    return active_temporal_upscaler.load(std::memory_order_relaxed);
}

} // namespace VideoCommon

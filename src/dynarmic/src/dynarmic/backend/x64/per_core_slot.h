// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>

namespace Dynarmic::Backend::X64 {

/// A pointer the shared code does not embed: each Jit that runs it keeps its own, in the Jit state
/// at the offset its PerCoreSlot names (see A64JitState::per_core).
/// A site that reads one of these must name its slot: the slot decides which value the running Jit
/// gets, never the value the founding Jit happened to have.
enum class PerCoreSlot : size_t {
    Callbacks,  ///< UserCallbacks of the running Jit
    JitImpl,    ///< Jit::Impl of the running Jit
    UserConfig, ///< UserConfig of the running Jit
    TpidrEl0,
    TpidrroEl0,
    FastDispatchTable,
    ExclusiveAddress,
    ExclusiveValue,
    Count,
};

inline constexpr size_t PerCoreSlotCount = size_t(PerCoreSlot::Count);

}  // namespace Dynarmic::Backend::X64
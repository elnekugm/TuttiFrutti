// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <vector>

#include "common/common_types.h"
#include "dynarmic/backend/x64/per_core_slot.h"
#include "dynarmic/backend/x64/xbyak.h"

namespace Dynarmic::Backend::X64 {

using RegList = std::vector<Xbyak::Reg64>;

class BlockOfCode;

class Callback {
public:
    virtual ~Callback();

    void EmitCall(BlockOfCode& code) const {
        EmitCall(code, [](RegList) {});
    }

    virtual void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const = 0;
    virtual void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const = 0;
};

class SimpleCallback final : public Callback {
public:
    template<typename Function>
    SimpleCallback(Function fn)
            : fn(reinterpret_cast<void (*)()>(fn)) {}

    using Callback::EmitCall;

    void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const override;
    void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const override;

private:
    void (*fn)();
};

class ArgCallback final : public Callback {
public:
    /// `slot` names the per-core value the call's object pointer is: shared code loads the running
    /// Jit's, never the founding Jit's.
    template<typename Function>
    ArgCallback(Function fn, u64 arg, PerCoreSlot slot = PerCoreSlot::Callbacks)
            : fn(reinterpret_cast<void (*)()>(fn)), arg(arg), slot(slot) {}

    using Callback::EmitCall;

    void EmitCall(BlockOfCode& code, std::function<void(RegList)> fn) const override;
    void EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> fn) const override;

    [[nodiscard]] u64 fn_address() const { return reinterpret_cast<u64>(fn); }

private:
    void (*fn)();
    u64 arg;
    PerCoreSlot slot;
};

}  // namespace Dynarmic::Backend::X64

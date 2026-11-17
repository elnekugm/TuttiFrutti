// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/x64/xbyak.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/x64/xbyak.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/callback.h"
#include "dynarmic/backend/x64/per_core_slot.h"
#include "dynarmic/backend/x64/constant_pool.h"
#include "dynarmic/backend/x64/host_feature.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/cond.h"

namespace Dynarmic::Backend::X64 {

using CodePtr = const void*;

struct RunCodeCallbacks {
    ArgCallback LookupBlock;
    ArgCallback AddTicks;
    ArgCallback GetTicksRemaining;
    bool enable_cycle_counting;
    /// Offset of a u64 in the JIT state holding the host stack pointer to restore whenever
    /// control is back in the dispatcher (A64 host_call_return); none when rsp never moves.
    std::optional<u32> host_stack_base_offset = std::nullopt;
};

/// Code shared by several Jits (A64 shared_code_cache): a pointer that differs between them, and
/// the offset in the JIT state where each Jit keeps its own.
struct PerCoreRegistration {
    PerCoreSlot slot;
    u32 offset;
};

class BlockOfCode final : public Xbyak::CodeGenerator {
public:
    /// With shared_code, `per_core` names the pointers of the Jit that builds the code which every
    /// Jit sharing it holds a value of its own for; more can be added before they are emitted.
    BlockOfCode(RunCodeCallbacks cb, JitStateInfo jsi, size_t total_code_size, std::function<void(BlockOfCode&)> rcp,
                bool shared_code = false, std::vector<PerCoreRegistration> per_core = {});
    BlockOfCode(const BlockOfCode&) = delete;

    /// Call when external emitters have finished emitting their preludes.
    void PreludeComplete();

    /// Change permissions to RW. This is required to support systems with W^X enforced.
    void EnableWriting();
    /// Change permissions to RX. This is required to support systems with W^X enforced.
    void DisableWriting();

    /// Clears this block of code and resets code pointer to beginning.
    void ClearCache();
    /// Calculates how much space is remaining to use.
    size_t SpaceRemaining() const;
    /// Ensure at least codesize bytes of code cache memory are committed at the current code_ptr.
    void EnsureMemoryCommitted(size_t codesize);

    /// Runs emulated code from code_ptr.
    HaltReason RunCode(void* jit_state, CodePtr code_ptr) const;
    /// Runs emulated code from code_ptr for a single cycle.
    HaltReason StepCode(void* jit_state, CodePtr code_ptr) const;
    /// Code emitter: Returns to dispatcher
    void ReturnFromRunCode(bool mxcsr_already_exited = false);
    /// Code emitter: Returns to dispatcher, forces return to host
    void ForceReturnFromRunCode(bool mxcsr_already_exited = false);
    /// Code emitter: Makes guest MXCSR the current MXCSR
    void SwitchMxcsrOnEntry();
    /// Code emitter: Makes saved host MXCSR the current MXCSR
    void SwitchMxcsrOnExit();
    /// Code emitter: Enter standard ASIMD MXCSR region
    void EnterStandardASIMD();
    /// Code emitter: Leave standard ASIMD MXCSR region
    void LeaveStandardASIMD();
    /// Code emitter: Updates cycles remaining my calling cb.AddTicks and cb.GetTicksRemaining
    /// @note this clobbers ABI caller-save registers
    void UpdateTicks();
    /// Code emitter: Performs a block lookup based on current state
    /// @note this clobbers ABI caller-save registers
    void LookupBlock();

    /// Code emitter: Load required flags for conditional cond from rax into host rflags
    void LoadRequiredFlagsForCondFromRax(IR::Cond cond);

    /// @brief Code emitter: Calls the function
    template<typename F>
        requires std::is_pointer_v<F> && std::is_function_v<std::remove_pointer_t<F>>
    void CallFunction(F fn) {
        ::Common::X64::CallFarFunction(*this, fn);
    }

    /// @brief Code emitter: Calls the lambda. Lambda must not have any captures.
    template<typename Lambda>
    void CallLambda(Lambda l) {
        ::Common::X64::CallFarFunction(*this, Common::FptrCast(l));
    }

    void ZeroExtendFrom(size_t bitsize, Xbyak::Reg64 reg) {
        switch (bitsize) {
        case 8:
            movzx(reg.cvt32(), reg.cvt8());
            return;
        case 16:
            movzx(reg.cvt32(), reg.cvt16());
            return;
        case 32:
            mov(reg.cvt32(), reg.cvt32());
            return;
        case 64:
            return;
        default:
            UNREACHABLE();
        }
    }

    Xbyak::Address Const(const Xbyak::AddressFrame& frame, u64 lower, u64 upper = 0);

    template<size_t esize>
    Xbyak::Address BConst(const Xbyak::AddressFrame& frame, u64 value) {
        return Const(frame, mcl::bit::replicate_element<u64>(esize, value),
                     mcl::bit::replicate_element<u64>(esize, value));
    }

    CodePtr GetCodeBegin() const;
    size_t GetTotalCodeSize() const;

    const void* GetReturnFromRunCodeAddress() const {
        return return_from_run_code[0];
    }

    const void* GetForceReturnFromRunCodeAddress() const {
        return return_from_run_code[FORCE_RETURN];
    }

    void int3() { db(0xCC); }

    /// Allocate memory of `size` bytes from the same block of memory the code is in.
    /// This is useful for objects that need to be placed close to or within code.
    /// The lifetime of this memory is the same as the code around it.
    void* AllocateFromCodeSpace(size_t size);

    void SetCodePtr(CodePtr code_ptr);
    void EnsurePatchLocationSize(CodePtr begin, size_t size);

    static const Xbyak::Reg64 ABI_JIT_PTR;
    // ABI registers
#ifdef _WIN32
    static const Xbyak::Reg64 ABI_RETURN;
    static const Xbyak::Reg64 ABI_PARAM1;
    static const Xbyak::Reg64 ABI_PARAM2;
    static const Xbyak::Reg64 ABI_PARAM3;
    static const Xbyak::Reg64 ABI_PARAM4;
    static const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> ABI_PARAMS;
#else
    static const Xbyak::Reg64 ABI_RETURN;
    static const Xbyak::Reg64 ABI_RETURN2;
    static const Xbyak::Reg64 ABI_PARAM1;
    static const Xbyak::Reg64 ABI_PARAM2;
    static const Xbyak::Reg64 ABI_PARAM3;
    static const Xbyak::Reg64 ABI_PARAM4;
    static const Xbyak::Reg64 ABI_PARAM5;
    static const Xbyak::Reg64 ABI_PARAM6;
    static const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> ABI_PARAMS;
#endif

    JitStateInfo GetJitStateInfo() const { return jsi; }

    bool HasHostFeature(HostFeature feature) const noexcept;

    // Flag reuse (A64 reuse_host_flags). The host flags equal the guest NZCV (x64 layout, CF in
    // ARM polarity) while getCurr() == nzcv_live_pos. nzcv_flags_pos/inst: the flags of the Add/Sub
    // that defined nzcv_flags_inst (a GetNZCVFromOp) are live while getCurr() == nzcv_flags_pos.
    const void* nzcv_live_pos = nullptr;
    const void* nzcv_flags_pos = nullptr;
    const void* nzcv_flags_inst = nullptr;

    // Address folding (A64 fold_address_offsets), per block: Add64(base, disp) instructions whose
    // only use is the address of a fastmem access. The Add is not emitted (its value is the base);
    // the access adds disp in its addressing mode.
    std::vector<std::pair<const void*, s32>> folded_address_adds;

    /// A64 elide_redundant_zext.
    bool elide_redundant_zext = false;

    /// Far code (A64 far_code): the last far_size bytes of the code space hold cold code. Call
    /// before PreludeComplete. Does nothing where the code space needs W^X switching.
    void EnableFarCode(size_t far_size);
    bool HasFarCode() const { return far_code_begin != nullptr; }
    void SwitchToFarCode();
    void SwitchToNearCode();
    /// Whether several Jits run this code (A64 shared_code_cache).
    bool SharedCode() const { return shared_code; }
    void AddPerCoreValue(PerCoreSlot slot, u32 offset) { per_core_offsets[size_t(slot)] = offset; }
    [[nodiscard]] bool HasPerCoreSlot(PerCoreSlot slot) const {
        return per_core_offsets[size_t(slot)] != 0;
    }
    /// Offset in the JIT state of a registered slot, or nullopt.
    [[nodiscard]] std::optional<u32> PerCoreOffset(PerCoreSlot slot) const {
        const u32 offset = per_core_offsets[size_t(slot)];
        return offset == 0 ? std::nullopt : std::optional<u32>{offset};
    }
    /// Code emitter: mov reg, value. In shared code, `slot` is loaded from the running Jit's state
    /// (ABI_JIT_PTR) instead, so the code is right for whichever Jit runs it. A shared-code site
    /// whose slot was not registered fails here: emitting the founding Jit's value would run the
    /// block with another Jit's state.
    void MovPerCore(const Xbyak::Reg64& reg, PerCoreSlot slot, u64 value);
    /// Shared code: the far code pointer, so a block's cold code can be checked too.
    const u8* FarCodePtr() const { return in_far_code ? getCurr<const u8*>() : far_code_ptr; }

    std::optional<s32> FoldedAddressDisp(const void* inst) const {
        for (const auto& [add, disp] : folded_address_adds) {
            if (add == inst) {
                return disp;
            }
        }
        return std::nullopt;
    }

private:
    using RunCodeFuncType = HaltReason (*)(void*, CodePtr);
    static constexpr size_t MXCSR_ALREADY_EXITED = 1 << 0;
    static constexpr size_t FORCE_RETURN = 1 << 1;

    // The commit and far-code state is declared before constant_pool on purpose: the ConstantPool
    // constructor already calls EnsureMemoryCommitted, which reads these, so they have to be
    // initialised first (members are initialised in declaration order).
#ifdef _WIN32
    size_t committed_size = 0;
#endif
    const u8* far_code_begin = nullptr;
    const u8* far_code_ptr = nullptr;
    const u8* near_code_ptr = nullptr;
    bool in_far_code = false;
#ifdef _WIN32
    size_t far_committed_end = 0;
#endif
    ConstantPool constant_pool;
    JitStateInfo jsi;
    std::array<const void*, 4> return_from_run_code;
    RunCodeFuncType run_code = nullptr;
    RunCodeFuncType step_code = nullptr;
    RunCodeCallbacks cb;
    CodePtr code_begin = nullptr;
    bool prelude_complete = false;
    bool shared_code = false;
    std::array<u32, PerCoreSlotCount> per_core_offsets{};  ///< 0: not registered

    void GenRunCode(std::function<void(BlockOfCode&)> rcp);
};

}  // namespace Dynarmic::Backend::X64

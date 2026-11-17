// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/halt_reason.h"

namespace Dynarmic {
namespace A64 {

enum class PrecompileResult {
    Compiled,   ///< Compiled now
    Present,    ///< Already in the code cache
    Unreadable, ///< Part of the block is not readable code: nothing was emitted
    Full,       ///< Less code space left than asked to keep free: nothing was emitted
};

/// Lets the Jits of one process share their compiled code: a block one of them compiles, every
/// one of them runs, instead of each compiling it into a cache of its own. Give each Jit the same
/// one in UserConfig::shared_code_cache. The configs may differ only in callbacks, processor_id,
/// tpidr_el0, tpidrro_el0 and counters, and must have compact_block_exits and no cycle counting;
/// a Jit that can't share keeps a cache of its own (Jit::SharesCode). Clearing or invalidating
/// the cache of one clears it for all, once none of them runs the code. x64 only; the other
/// backends keep a cache per Jit.
class SharedCodeCache final {
public:
    /// With verify, every block is checked for pointers of the Jit that compiled it, which the
    /// shared code must load from the running Jit instead (VerifyFailures counts the ones found).
    /// With stats (EDEN_NX2_SHARED_JIT_STATS), block lookups and the time they wait for and hold
    /// compile_mutex are counted.
    explicit SharedCodeCache(bool verify = false, bool stats = false);
    ~SharedCodeCache();

    SharedCodeCache(const SharedCodeCache&) = delete;
    SharedCodeCache& operator=(const SharedCodeCache&) = delete;

    /// Blocks found holding another Jit's pointer, with verify.
    std::size_t VerifyFailures() const;
    /// Whole-cache clears so far.
    std::size_t Clears() const;

    /// What GetSharedBlock did, with stats: lookups, hits, compiles, and the time they spent
    /// waiting for and holding compile_mutex. All zero when statistics are off.
    struct Stats {
        std::uint64_t lookups{};
        std::uint64_t hits{};
        std::uint64_t compiles{};
        std::uint64_t lock_wait_ns{};
        std::uint64_t lock_hold_ns{};
        std::uint64_t lock_max_wait_ns{};
    };
    [[nodiscard]] Stats GetStats() const;

    /// How many per-core slots the shared code reads from the running Jit (diagnostic: a joined
    /// Jit registers all of them, so a shared-code site with an unregistered slot would assert).
    [[nodiscard]] std::size_t RegisteredSlots() const;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

class Jit final {
public:
    explicit Jit(UserConfig conf);
    ~Jit();

    /**
     * Runs the emulated CPU.
     * Cannot be recursively called.
     */
    HaltReason Run();

    /**
     * Step the emulated CPU for one instruction.
     * Cannot be recursively called.
     */
    HaltReason Step();

    /**
     * Clears the code cache of all compiled code.
     * Can be called at any time. Halts execution if called within a callback.
     */
    void ClearCache();

    /**
     * Invalidate the code cache at a range of addresses.
     * @param start_address The starting address of the range to invalidate.
     * @param length The length (in bytes) of the range to invalidate.
     */
    void InvalidateCacheRange(std::uint64_t start_address, std::size_t length);

    /**
     * Compiles the block at location (an IR::LocationDescriptor value) as if execution had
     * reached it, without running anything. Leaves the guest state alone.
     * Only from the thread that calls Run, while Run is not executing.
     * Pending ClearCache and InvalidateCacheRange requests stay pending: the next Run applies them
     * to the blocks compiled here as well. Never clears the cache to make room.
     * @param keep_free Code space (bytes) to leave free: Full when the block could not.
     */
    PrecompileResult Precompile(std::uint64_t location, std::size_t keep_free);

    /// Bytes of the code cache in use.
    std::size_t CodeCacheUsed() const;

    /// Whether this Jit runs the code of a SharedCodeCache.
    bool SharesCode() const;

    /**
     * Reset CPU state to state at startup. Does not clear code cache.
     * Cannot be called from a callback.
     */
    void Reset();

    /**
     * Stops execution in Jit::Run.
     */
    void HaltExecution(HaltReason hr = HaltReason::UserDefined1);

    /**
     * Clears a halt reason from flags.
     * Warning: Only use this if you're sure this won't introduce races.
     */
    void ClearHalt(HaltReason hr = HaltReason::UserDefined1);

    /// Read Stack Pointer
    std::uint64_t GetSP() const;
    /// Modify Stack Pointer
    void SetSP(std::uint64_t value);

    /// Read Program Counter
    std::uint64_t GetPC() const;
    /// Modify Program Counter
    void SetPC(std::uint64_t value);

    /// Read general-purpose register.
    std::uint64_t GetRegister(std::size_t index) const;
    /// Modify general-purpose register.
    void SetRegister(size_t index, std::uint64_t value);

    /// Read all general-purpose registers.
    std::array<std::uint64_t, 31> GetRegisters() const;
    /// Modify all general-purpose registers.
    void SetRegisters(const std::array<std::uint64_t, 31>& value);

    /// Read floating point and SIMD register.
    Vector GetVector(std::size_t index) const;
    /// Modify floating point and SIMD register.
    void SetVector(std::size_t index, Vector value);

    /// Read all floating point and SIMD registers.
    std::array<Vector, 32> GetVectors() const;
    /// Modify all floating point and SIMD registers.
    void SetVectors(const std::array<Vector, 32>& value);

    /// View FPCR.
    std::uint32_t GetFpcr() const;
    /// Modify FPCR.
    void SetFpcr(std::uint32_t value);

    /// View FPSR.
    std::uint32_t GetFpsr() const;
    /// Modify FPSR.
    void SetFpsr(std::uint32_t value);

    /// View PSTATE
    std::uint32_t GetPstate() const;
    /// Modify PSTATE
    void SetPstate(std::uint32_t value);

    /// Clears exclusive state for this core.
    void ClearExclusiveState();

    /**
     * Returns true if Jit::Run was called but hasn't returned yet.
     * i.e.: We're in a callback.
     */
    bool IsExecuting() const;

    /// @brief Disassemble the instructions following the current pc and return
    /// the resulting instructions as a vector of their string representations.
    std::string Disassemble() const;

    /// Backend state (SharedCodeCache keeps a list of the Jits sharing its code).
    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

}  // namespace A64
}  // namespace Dynarmic

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <vector>

#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "common/container/unordered_set.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include <bit>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::A64 {

using namespace Backend::X64;

namespace {

/// One clock for the optional shared-cache statistics.
u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

}  // namespace

static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf, bool shared) {
    const ArgCallback add_ticks = Devirtualize<&A64::UserCallbacks::AddTicks>(cb);
    const ArgCallback ticks_remaining = Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(cb);
    // Shared code: the lookup and the callbacks belong to the Jit that runs the block, so only
    // their slot is kept; the founding Jit's pointers are not stored in it.
    return RunCodeCallbacks{
        ArgCallback(LookupBlock, shared ? 0 : reinterpret_cast<u64>(arg), PerCoreSlot::JitImpl),
        ArgCallback(add_ticks.fn_address(), shared ? 0 : u64(cb), PerCoreSlot::Callbacks),
        ArgCallback(ticks_remaining.fn_address(), shared ? 0 : u64(cb), PerCoreSlot::Callbacks),
        conf.enable_cycle_counting,
        HostCallReturn(conf) ? std::optional<u32>{u32(offsetof(A64JitState, host_stack_base))} : std::nullopt,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
        if (HostCallReturn(conf)) {
            // A RET compares its target with the frame's [rsp + ABI_SHADOW_SPACE]; with no guest
            // frame that is the first StackLayout word (the spills live in the JIT state), which
            // holds a value no location descriptor has (bits 57-58 are never set in one).
            code.mov(code.qword[code.rsp + ABI_SHADOW_SPACE], -1);
            code.mov(code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, host_stack_base)], code.rsp);
            code.lea(code.rax, code.ptr[code.rsp - HostCallFrameSize * HostCallMaxDepth]);
            code.mov(code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, host_stack_limit)], code.rax);
        }
    };
}

// Largest power of two <= conf.rsb_size, between 8 and A64JitState::RSB_SIZE, minus one.
static size_t RsbPtrMask(const A64::UserConfig& conf) {
    size_t size = A64JitState::RSB_SIZE;
    while (size > 8 && size > conf.rsb_size) {
        size >>= 1;
    }
    return size - 1;
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

/// The code, emitter and state of a SharedCodeCache, made by the first Jit that joins it.
struct SharedCodeCache::Impl {
    Impl(bool verify, bool stats, bool fast_lookup)
        : verify(verify), stats(stats), fast_lookup(fast_lookup) {}

    const bool verify;
    /// EDEN_NX2_SHARED_JIT_STATS: GetSharedBlock counts lookups, hits, compiles and the time it
    /// waited for and held compile_mutex.
    const bool stats;
    /// Lookups use the per-Jit table and the shared index read lock instead of compile_mutex.
    const bool fast_lookup;

    // Made by the first Jit (Jit::Impl::JoinShared), whose config builds the code.
    std::optional<UserConfig> conf;
    std::unique_ptr<BlockOfCode> code;
    std::unique_ptr<A64EmitX64> emitter;
    Optimization::PolyfillOptions polyfill_options{};
    std::vector<u64> founder_values;  ///< The founding Jit's per-core pointers, for verify

    /// Taken to compile into the code, and by an invalidation (after `mutex`).
    std::mutex compile_mutex;

    /// The code is only cleared or invalidated while no Jit runs it (inside == 0).
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Jit::Impl*> members;
    size_t inside = 0;
    bool pending = false;
    bool clear_all = false;
    boost::icl::interval_set<u64> ranges;
    ::Common::unordered_set<IR::LocationDescriptor> blocks;
    /// Bumped by each invalidation: a Jit drops its RSB and fast dispatch entries when it sees it.
    u64 epoch = 0;

    std::atomic<size_t> verify_failures{0};
    std::atomic<size_t> clears{0};

    // EDEN_NX2_SHARED_JIT_STATS
    std::atomic<u64> lookups{0};
    std::atomic<u64> hits{0};
    std::atomic<u64> compiles{0};
    std::atomic<u64> lock_wait_ns{0};
    std::atomic<u64> lock_hold_ns{0};
    std::atomic<u64> lock_max_wait_ns{0};

    void AddLockWait(u64 ns) {
        lock_wait_ns.fetch_add(ns, std::memory_order_relaxed);
        u64 last = lock_max_wait_ns.load(std::memory_order_relaxed);
        while (ns > last && !lock_max_wait_ns.compare_exchange_weak(last, ns, std::memory_order_relaxed)) {
        }
    }
    void AddLockHold(u64 ns) { lock_hold_ns.fetch_add(ns, std::memory_order_relaxed); }
    void AddLookup(bool hit, u64 hold_ns) {
        lookups.fetch_add(1, std::memory_order_relaxed);
        if (hit) {
            hits.fetch_add(1, std::memory_order_relaxed);
        } else {
            compiles.fetch_add(1, std::memory_order_relaxed);
        }
        AddLockHold(hold_ns);
    }
};

SharedCodeCache::SharedCodeCache(bool verify, bool stats, bool fast_lookup)
        : impl(std::make_unique<Impl>(verify, stats, fast_lookup)) {}

SharedCodeCache::~SharedCodeCache() = default;

std::size_t SharedCodeCache::VerifyFailures() const {
    return impl->verify_failures.load(std::memory_order_relaxed);
}

std::size_t SharedCodeCache::Clears() const {
    return impl->clears.load(std::memory_order_relaxed);
}

SharedCodeCache::Stats SharedCodeCache::GetStats() const {
    Stats stats;
    stats.lookups = impl->lookups.load(std::memory_order_relaxed);
    stats.hits = impl->hits.load(std::memory_order_relaxed);
    stats.compiles = impl->compiles.load(std::memory_order_relaxed);
    stats.lock_wait_ns = impl->lock_wait_ns.load(std::memory_order_relaxed);
    stats.lock_hold_ns = impl->lock_hold_ns.load(std::memory_order_relaxed);
    stats.lock_max_wait_ns = impl->lock_max_wait_ns.load(std::memory_order_relaxed);
    return stats;
}

std::size_t SharedCodeCache::RegisteredSlots() const {
    std::scoped_lock lock{impl->mutex};
    if (!impl->code) {
        return 0;
    }
    size_t registered = 0;
    for (size_t slot = 0; slot < PerCoreSlotCount; ++slot) {
        registered += impl->code->HasPerCoreSlot(PerCoreSlot(slot)) ? 1 : 0;
    }
    return registered;
}

namespace {

/// What of a config shapes the code: Jits whose configs differ in anything else can't share it.
auto SharedCodeKey(const UserConfig& c) {
    return std::make_tuple(
        c.fastmem_pointer, c.global_monitor, c.tpidrro_el0 != nullptr, c.tpidr_el0 != nullptr, c.page_table,
        c.optimizations, c.page_table_address_space_bits, c.page_table_pointer_mask, c.page_table_log2_stride,
        c.page_table_marked_bit, c.page_table_sign_extension, c.cntfrq_el0, c.ctr_el0, c.dczid_el0,
        c.fastmem_address_space_bits, c.code_cache_size, c.detect_misaligned_access_via_page_table,
        c.unsafe_optimizations, c.hook_data_cache_operations, c.hook_isb, c.hook_hint_instructions,
        c.silently_mirror_page_table, c.absolute_offset_page_table,
        c.only_detect_misalignment_via_page_table_on_page_boundary, c.recompile_on_fastmem_failure,
        c.silently_mirror_fastmem, c.fastmem_exclusive_access, c.recompile_on_exclusive_fastmem_failure,
        c.spill_to_free_gpr, c.scratch_copy_to_free_reg, c.fold_address_offsets, c.elide_redundant_zext,
        c.omit_block_frame, c.halt_check_on_back_edges, c.rsb_size, c.compact_block_exits, c.far_code,
        c.fast_dispatch_table_bits, c.fast_dispatch_set_associative, c.reuse_host_flags, c.host_call_return,
        c.define_unpredictable_behaviour, c.wall_clock_cntpct, c.check_halt_on_memory_access,
        c.enable_cycle_counting, c.very_verbose_debugging_output);
}

/// Whether code built from this config can run on several Jits at once. Patches rewrite only the
/// rel32 of compact exits; cycle counting calls back into one Jit's tick counters.
bool CanShareCode([[maybe_unused]] const UserConfig& conf) {
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
    return false;
#else
    return conf.shared_code_cache && conf.compact_block_exits && !conf.enable_cycle_counting &&
           conf.callbacks != nullptr;
#endif
}

}  // namespace

struct Jit::Impl final {
public:
    Impl(Jit* jit, UserConfig conf_)
        : conf(conf_)
    {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
        if (CanShareCode(conf)) {
            JoinShared();
        }
        if (!shared) {
            own_code = std::make_unique<BlockOfCode>(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf, false), JitStateInfo{jit_state, RsbPtrMask(conf)}, conf.code_cache_size, GenRCP(conf));
            own_emitter = std::make_unique<A64EmitX64>(*own_code, conf, jit);
            block_of_code = own_code.get();
            emitter = own_emitter.get();
            polyfill_options = GenPolyfillOptions(*own_code);
        }
    }

    ~Impl() {
        if (shared) {
            std::scoped_lock lock{shared->mutex};
            std::erase(shared->members, this);
        }
    }

    HaltReason Run() {
        ASSERT(!is_executing);
        if (shared) {
            EnterShared();
        } else {
            PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        }
        is_executing = true;
        // TODO: Check code alignment
        const CodePtr current_code_ptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & u32(RsbPtrMask(conf));
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return CodePtr(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }
            return CodePtr((uintptr_t(GetCurrentBlock()) + 15) & ~uintptr_t(15));
        }();
        const HaltReason hr = block_of_code->RunCode(&jit_state, current_code_ptr);
        if (shared) {
            LeaveShared();
        } else {
            PerformRequestedCacheInvalidation(hr);
        }
        is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        if (shared) {
            EnterShared();
        } else {
            PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        }
        is_executing = true;
        const HaltReason hr = block_of_code->StepCode(&jit_state, GetCurrentSingleStep());
        if (shared) {
            LeaveShared();
        } else {
            PerformRequestedCacheInvalidation(hr);
        }
        is_executing = false;
        return hr;
    }

    void ClearCache() {
        if (shared) {
            RequestShared([](SharedCodeCache::Impl& s) { s.clear_all = true; });
            return;
        }
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        if (shared) {
            RequestShared([&range](SharedCodeCache::Impl& s) { s.ranges.add(range); });
            return;
        }
        std::unique_lock lock{invalidation_mutex};
        invalid_cache_ranges.add(range);
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        const auto per_core = jit_state.per_core;
        jit_state = {};
        jit_state.per_core = per_core;
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    std::string Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code->getCurr()) - reinterpret_cast<const char*>(block_of_code->GetCodeBegin());
        auto const* p = reinterpret_cast<const char*>(block_of_code->GetCodeBegin());
        return Common::DisassembleX64(p, p + size);
    }

    PrecompileResult Precompile(std::uint64_t location, std::size_t keep_free) {
        ASSERT(!is_executing);
        const IR::LocationDescriptor descriptor{location};
        if (A64::LocationDescriptor{descriptor}.SingleStepping()) {
            return PrecompileResult::Unreadable;
        }
        std::unique_lock compile_lock{CompileMutex(), std::defer_lock};
        if (shared) {
            compile_lock.lock();
        }
        if (emitter->GetBasicBlock(descriptor)) {
            return PrecompileResult::Present;
        }
        // Unlike GetBlock, never clears the cache to make room. GetBlock assumes a block fits in
        // MINIMUM_REMAINING_CODESIZE, so keep_free is left after this one.
        if (block_of_code->SpaceRemaining() < MINIMUM_REMAINING_CODESIZE + keep_free) {
            return PrecompileResult::Full;
        }
        block_of_code->EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);
        if (!CompileBlock(descriptor, true)) {
            return PrecompileResult::Unreadable;
        }
        ASSERT(block_of_code->SpaceRemaining() >= keep_free);
        return PrecompileResult::Compiled;
    }

    size_t CodeCacheUsed() const {
        return size_t(reinterpret_cast<const char*>(block_of_code->getCurr()) - reinterpret_cast<const char*>(block_of_code->GetCodeBegin()));
    }

    bool SharesCode() const {
        return shared != nullptr;
    }

private:
    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        return this_->GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        if (conf.counters) {
            ++conf.counters->block_lookups;
        }
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    static constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;

    CodePtr GetBlock(IR::LocationDescriptor descriptor) {
        if (shared) {
            return GetSharedBlock(descriptor);
        }
        if (auto block = emitter->GetBasicBlock(descriptor))
            return block->entrypoint;

        if (block_of_code->SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            // Immediately evacuate cache
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }
        block_of_code->EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        // JIT Compile
        const CodePtr entrypoint = CompileBlock(descriptor, false);
        conf.callbacks->BlockCompiled(descriptor.Value());
        return entrypoint;
    }

    /// Translates, optimizes and emits the block. With skip_unreadable, returns nullptr and emits
    /// nothing when an instruction of the block could not be read.
    CodePtr CompileBlock(IR::LocationDescriptor descriptor, bool skip_unreadable) {
        bool readable = true;
        const auto get_code = [this, &readable](u64 vaddr) {
            const std::optional<u32> instruction = conf.callbacks->MemoryReadCode(vaddr);
            readable = readable && instruction.has_value();
            return instruction;
        };
        // LocationDescriptor ctor() does important ops (like tflags) do not skip
        auto const arch_descriptor = A64::LocationDescriptor{descriptor};
        ir_block.Reset(arch_descriptor);
        A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
        if (skip_unreadable && !readable) {
            return nullptr;
        }
        Optimization::Optimize(ir_block, conf, polyfill_options);
        if (conf.counters) {
            ++conf.counters->blocks_compiled;
        }
        if (shared && shared->verify) {
            const u8* const near_begin = block_of_code->getCurr<const u8*>();
            const u8* const far_begin = block_of_code->FarCodePtr();
            const CodePtr entrypoint = emitter->Emit(ir_block).entrypoint;
            VerifySharedCode(near_begin, block_of_code->getCurr<const u8*>());
            if (far_begin) {
                VerifySharedCode(far_begin, block_of_code->FarCodePtr());
            }
            return entrypoint;
        }
        return emitter->Emit(ir_block).entrypoint;
    }

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

            jit_state.ResetRSB();
            if (invalidate_entire_cache) {
                if (conf.counters) {
                    ++conf.counters->cache_clears;
                }
                block_of_code->ClearCache();
                emitter->ClearCache();
            } else {
                emitter->InvalidateCacheRanges(invalid_cache_ranges);
            }
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

    // ---- Shared code (UserConfig::shared_code_cache) ----

    std::mutex& CompileMutex() {
        return shared ? shared->compile_mutex : invalidation_mutex;
    }

    /// Makes the shared code from this Jit's config if it is the first to join, else checks that
    /// its config can run that code. Leaves `shared` null when it can't.
    void JoinShared() {
        SharedCodeCache::Impl& s = *conf.shared_code_cache->impl;
        std::scoped_lock lock{s.compile_mutex, s.mutex};
        if (!s.code) {
            std::vector<PerCoreRegistration> registrations;
            const auto add = [&registrations](PerCoreSlot slot) {
                registrations.push_back({slot, A64JitState::PerCoreOffset(slot)});
            };
            add(PerCoreSlot::Callbacks);
            add(PerCoreSlot::JitImpl);
            add(PerCoreSlot::TpidrEl0);
            add(PerCoreSlot::TpidrroEl0);
            if (conf.global_monitor) {
                add(PerCoreSlot::ExclusiveAddress);
                add(PerCoreSlot::ExclusiveValue);
            }
            // The shared state keeps what shapes code generation, never the founding Jit's
            // callbacks: a member may compile after this Jit is destroyed, and the emitted calls
            // read the running Jit's callbacks from its state instead.
            s.conf = conf;
            s.conf->callbacks = nullptr;
            s.conf->shared_code_cache.reset();  // the cache holds its own config: no cycle
            s.code = std::make_unique<BlockOfCode>(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf, true), JitStateInfo{jit_state, RsbPtrMask(conf)}, conf.code_cache_size, GenRCP(conf), true, std::move(registrations));
            s.emitter = std::make_unique<A64EmitX64>(*s.code, *s.conf, nullptr, conf.callbacks);
            s.polyfill_options = GenPolyfillOptions(*s.code);
            // Pointers a block must never embed (verify). Not the callbacks pointer: emitted calls
            // load it from the running Jit. Not the exclusive address slots: the monitor clear
            // embeds every processor's and skips the running one at run time.
            s.founder_values.push_back(u64(this));
            if (conf.tpidr_el0) {
                s.founder_values.push_back(u64(conf.tpidr_el0));
            }
            if (conf.tpidrro_el0) {
                s.founder_values.push_back(u64(conf.tpidrro_el0));
            }
            if (conf.global_monitor) {
                s.founder_values.push_back(u64(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));
            }
            SharedCodeCache::Impl* const s_ptr = &s;
            s.emitter->request_invalidation = [s_ptr](IR::LocationDescriptor location) {
                RequestShared(*s_ptr, [location](SharedCodeCache::Impl& c) { c.blocks.insert(location); });
            };
        } else if (SharedCodeKey(*s.conf) != SharedCodeKey(conf)) {
            return;
        }
        shared = &s;
        block_of_code = s.code.get();
        emitter = s.emitter.get();
        polyfill_options = s.polyfill_options;

        if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
            fast_dispatch_table.resize(emitter->fast_dispatch_table.size());
        }
        const auto set = [this](PerCoreSlot slot, u64 value) {
            jit_state.per_core[size_t(slot)] = value;
        };
        set(PerCoreSlot::Callbacks, u64(conf.callbacks));
        set(PerCoreSlot::JitImpl, u64(this));
        set(PerCoreSlot::UserConfig, u64(&conf));
        set(PerCoreSlot::TpidrEl0, u64(conf.tpidr_el0));
        set(PerCoreSlot::TpidrroEl0, u64(conf.tpidrro_el0));
        set(PerCoreSlot::FastDispatchTable, u64(fast_dispatch_table.data()));
        if (conf.global_monitor) {
            set(PerCoreSlot::ExclusiveAddress, u64(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)));
            set(PerCoreSlot::ExclusiveValue, u64(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));
        }
        s.members.push_back(this);
        seen_epoch = s.epoch;
    }

    /// Records a change to the shared code and halts every Jit; it is made once none runs the code.
    template<typename Change>
    static void RequestShared(SharedCodeCache::Impl& s, Change&& change) {
        std::unique_lock lock{s.mutex};
        change(s);
        s.pending = true;
        for (Jit::Impl* const member : s.members) {
            member->HaltExecution(HaltReason::CacheInvalidation);
        }
        if (s.inside == 0) {
            PerformShared(s);
        }
    }

    template<typename Change>
    void RequestShared(Change&& change) {
        RequestShared(*shared, std::forward<Change>(change));
    }

    /// Under s.mutex, with no Jit in the code.
    static void PerformShared(SharedCodeCache::Impl& s) {
        ASSERT(s.inside == 0);
        std::scoped_lock compile_lock{s.compile_mutex};
        if (s.clear_all) {
            s.clears.fetch_add(1, std::memory_order_relaxed);
            s.code->ClearCache();
            s.emitter->ClearCache();
        } else {
            if (!s.ranges.empty()) {
                s.emitter->InvalidateCacheRanges(s.ranges);
            }
            if (!s.blocks.empty()) {
                s.emitter->InvalidateBasicBlocks(s.blocks);
            }
        }
        s.clear_all = false;
        s.ranges.clear();
        s.blocks.clear();
        s.pending = false;
        ++s.epoch;
        s.cv.notify_all();
    }

    /// Before running the code: waits out a pending change, then drops what this Jit kept of the
    /// code from before the last one (its RSB and fast dispatch entries).
    void EnterShared() {
        std::unique_lock lock{shared->mutex};
        ClearHalt(HaltReason::CacheInvalidation);
        while (shared->pending) {
            if (shared->inside == 0) {
                PerformShared(*shared);
            } else {
                shared->cv.wait(lock);
            }
        }
        ++shared->inside;
        SyncSharedEpoch();
    }

    /// Every guest SVC leaves the code (tens of thousands a second per core): no wake-up unless a
    /// change is made, which PerformShared announces itself.
    void LeaveShared() {
        std::unique_lock lock{shared->mutex};
        --shared->inside;
        if (shared->pending && shared->inside == 0) {
            PerformShared(*shared);
        }
    }

    /// The shared code is full: clears it, waiting for the other Jits to leave it. This Jit keeps
    /// no pointer into the code meanwhile (the dispatcher dropped its host call frames before the
    /// lookup), so it counts as outside while it waits.
    void ClearSharedFromInside() {
        std::unique_lock lock{shared->mutex};
        shared->clear_all = true;
        shared->pending = true;
        for (Jit::Impl* const member : shared->members) {
            member->HaltExecution(HaltReason::CacheInvalidation);
        }
        --shared->inside;
        while (shared->pending) {
            if (shared->inside == 0) {
                PerformShared(*shared);
            } else {
                shared->cv.notify_all();
                shared->cv.wait(lock);
            }
        }
        ++shared->inside;
        ClearHalt(HaltReason::CacheInvalidation);
        SyncSharedEpoch();
        if (conf.counters) {
            ++conf.counters->cache_clears;
        }
    }

    /// Under shared->mutex.
    void SyncSharedEpoch() {
        if (seen_epoch == shared->epoch) {
            return;
        }
        seen_epoch = shared->epoch;
        jit_state.ResetRSB();
        std::fill(fast_dispatch_table.begin(), fast_dispatch_table.end(), A64EmitX64::FastDispatchEntry{});
        std::fill(shared_lookup.begin(), shared_lookup.end(), SharedLookupEntry{});
    }

    CodePtr GetSharedBlock(IR::LocationDescriptor descriptor) {
        // EDEN_NX2_SHARED_JIT_STATS: one branch per lookup, nothing when off.
        const bool stats = shared->stats;
        const u64 location = descriptor.Value();

        // Hits: this Jit's own table, no lock and no shared state. An entry is valid only while
        // the epoch it was filled in is current. An invalidation or clear waits for every Jit to
        // leave the code (inside == 0) before it runs, and that happens after this Run returns;
        // the next Enter then fails SyncSharedEpoch and empties this table.
        if (shared->fast_lookup) {
            const SharedLookupEntry& cached = shared_lookup[SharedLookupIndex(location)];
            if (cached.code != nullptr && cached.location == location && cached.epoch == seen_epoch) {
                if (stats) {
                    shared->AddLookup(true, 0);
                }
                return cached.code;
            }
            // The shared index under a read lock: block lookups on different Jits no longer
            // serialize on compile_mutex. Entries are only erased while no Jit runs the code.
            if (const auto block = emitter->GetBasicBlock(descriptor)) {
                shared_lookup[SharedLookupIndex(location)] = {location, block->entrypoint, seen_epoch};
                if (stats) {
                    shared->AddLookup(true, 0);
                }
                return block->entrypoint;
            }
        }

        // Miss: compile, exclusively, after re-checking (another Jit may have compiled it).
        const u64 wait_begin = stats ? NowNs() : 0;
        CodePtr entrypoint = nullptr;
        bool compiled = false;
        {
            std::unique_lock compile_lock{shared->compile_mutex};
            u64 hold_begin = 0;
            if (stats) {
                hold_begin = NowNs();
                shared->AddLockWait(hold_begin - wait_begin);
            }
            // Cleared at most once, like the private cache: after that the block is compiled anyway.
            bool cleared = false;
            while (true) {
                if (auto block = emitter->GetBasicBlock(descriptor)) {
                    entrypoint = block->entrypoint;
                    break;
                }
                if (cleared || block_of_code->SpaceRemaining() >= MINIMUM_REMAINING_CODESIZE) {
                    break;
                }
                if (stats) {
                    shared->AddLockHold(NowNs() - hold_begin);
                }
                compile_lock.unlock();
                ClearSharedFromInside();
                compile_lock.lock();
                if (stats) {
                    hold_begin = NowNs();
                }
                cleared = true;
            }
            if (entrypoint == nullptr) {
                block_of_code->EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);
                entrypoint = CompileBlock(descriptor, false);
                compiled = true;
            }
            if (stats) {
                shared->AddLookup(!compiled, NowNs() - hold_begin);
            }
        }
        if (shared->fast_lookup) {
            shared_lookup[SharedLookupIndex(location)] = {location, entrypoint, seen_epoch};
        }
        // Only a block compiled here is news to the warm-up: one that already existed was recorded
        // when it was compiled (by this Jit or another), and its lock on every lookup is not free.
        if (compiled) {
            conf.callbacks->BlockCompiled(location);
        }
        return entrypoint;
    }

    /// Verify: a pointer of the Jit that built the shared code, embedded in a block, would be used
    /// by every Jit that runs it.
    void VerifySharedCode(const u8* begin, const u8* end) {
        if (end <= begin) {
            return;
        }
        for (const u64 value : shared->founder_values) {
            u8 bytes[sizeof(u64)];
            std::memcpy(bytes, &value, sizeof(value));
            if (std::search(begin, end, std::begin(bytes), std::end(bytes)) != end) {
                shared->verify_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    IR::Block ir_block = {LocationDescriptor(0, FP::FPCR(0), false)};
    const UserConfig conf;
    A64JitState jit_state;
    std::unique_ptr<BlockOfCode> own_code;
    std::unique_ptr<A64EmitX64> own_emitter;
    BlockOfCode* block_of_code = nullptr;
    A64EmitX64* emitter = nullptr;
    Optimization::PolyfillOptions polyfill_options{};
    bool is_executing = false;
    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::mutex invalidation_mutex;

    SharedCodeCache::Impl* shared = nullptr;
    std::vector<A64EmitX64::FastDispatchEntry> fast_dispatch_table;  ///< Shared code: this Jit's
    u64 seen_epoch = 0;

    /// Shared code, fast lookups: a direct-mapped map from block location to its code, filled by
    /// lookups and by compiles, tagged with the epoch it was filled in. Only this Jit's thread
    /// touches it, so a hit costs no lock and no shared state. It is emptied by SyncSharedEpoch,
    /// and an entry whose epoch is not current is ignored.
    struct SharedLookupEntry {
        u64 location = ~u64{0};
        CodePtr code = nullptr;
        u64 epoch = 0;
    };
    static constexpr size_t SharedLookupBits = 12;
    static constexpr size_t SharedLookupSize = size_t{1} << SharedLookupBits;
    static size_t SharedLookupIndex(u64 location) {
        return size_t((location * 0x9E37'79B9'7F4A'7C15ull) >> (64 - SharedLookupBits));
    }
    std::array<SharedLookupEntry, SharedLookupSize> shared_lookup{};
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

PrecompileResult Jit::Precompile(std::uint64_t location, std::size_t keep_free) {
    return impl->Precompile(location, keep_free);
}

std::size_t Jit::CodeCacheUsed() const {
    return impl->CodeCacheUsed();
}

bool Jit::SharesCode() const {
    return impl->SharesCode();
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64

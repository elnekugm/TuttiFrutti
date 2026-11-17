// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "dynarmic/interface/A64/a64.h"

#ifdef ARCHITECTURE_x86_64

namespace {

using Dynarmic::A64::Jit;
using Dynarmic::A64::PrecompileResult;
using Dynarmic::A64::SharedCodeCache;

constexpr u64 Entry = 0x10000000;
constexpr u32 Svc0 = 0xD4000001;
constexpr u32 Ret = 0xD65F03C0;

constexpr u32 Movz(u32 rd, u32 value) {
    return 0xD2800000U | (value << 5) | rd;
}
constexpr u32 AddReg(u32 rd, u32 rn, u32 rm) {
    return 0x8B000000U | (rm << 16) | (rn << 5) | rd;
}
constexpr u32 AddImm(u32 rd, u32 rn, u32 imm) {
    return 0x91000000U | (imm << 10) | (rn << 5) | rd;
}
constexpr u32 SubsImm(u32 rd, u32 rn, u32 imm) {
    return 0xF1000000U | (imm << 10) | (rn << 5) | rd;
}
constexpr u32 LdrX(u32 rt, u32 rn) {
    return 0xF9400000U | (rn << 5) | rt;
}
constexpr u32 MrsTpidrEl0(u32 rt) {
    return 0xD53BD040U | rt;
}
constexpr u32 Bl(s32 offset) {
    return 0x94000000U | (u32(offset / 4) & 0x3FFFFFF);
}
constexpr u32 B(s32 offset) {
    return 0x14000000U | (u32(offset / 4) & 0x3FFFFFF);
}
constexpr u32 BNe(s32 offset) {
    return 0x54000001U | ((u32(offset / 4) & 0x7FFFF) << 5);
}
constexpr u32 Br(u32 rn) {
    return 0xD61F0000U | (rn << 5);
}
constexpr u32 MovzHw(u32 rd, u32 imm16, u32 hw) {
    return 0xD2800000U | (hw << 21) | ((imm16 & 0xFFFF) << 5) | rd;
}
constexpr u32 MovkHw(u32 rd, u32 imm16, u32 hw) {
    return 0xF2800000U | (hw << 21) | ((imm16 & 0xFFFF) << 5) | rd;
}


using Code = std::map<u64, u32>;

/// `blocks` blocks of `movz/movk x0, next; br x0`, then svc: every transition goes through the
/// dispatcher (an indirect branch is never linked), so a Run does one shared-code lookup per
/// block. Used by the benchmark to measure the lookup path with Run entry/exit amortized.
Code IndirectChainProgram(u32 blocks) {
    Code code;
    constexpr u64 BlockSize = 12;
    for (u32 i = 0; i < blocks; ++i) {
        const u64 base = Entry + i * BlockSize;
        const u64 next = i + 1 < blocks ? base + BlockSize : Entry + blocks * BlockSize;
        code[base + 0] = MovzHw(9, u32(next & 0xFFFF), 0);
        code[base + 4] = MovkHw(9, u32((next >> 16) & 0xFFFF), 1);
        code[base + 8] = Br(9);
    }
    code[Entry + blocks * BlockSize] = Svc0;
    return code;
}

/// x0 = iterations * (TPIDR_EL0 + [x4] + 1): TPIDR_EL0 and the load go to the running Jit's
/// pointer and callbacks, and the +1 is a BL/RET pair.
Code LoopProgram(u32 iterations) {
    return {
        {Entry + 0, Movz(0, 0)},
        {Entry + 4, Movz(1, iterations)},
        {Entry + 8, Movz(4, 0x100)},
        {Entry + 12, MrsTpidrEl0(2)},  // loop:
        {Entry + 16, AddReg(0, 0, 2)},
        {Entry + 20, LdrX(3, 4)},
        {Entry + 24, AddReg(0, 0, 3)},
        {Entry + 28, Bl(44 - 28)},
        {Entry + 32, SubsImm(1, 1, 1)},
        {Entry + 36, BNe(12 - 36)},
        {Entry + 40, Svc0},
        {Entry + 44, AddImm(0, 0, 1)},  // func:
        {Entry + 48, Ret},
    };
}

/// `blocks` blocks of `add x0, x0, #1; b next`, then svc: x0 = blocks.
Code ChainProgram(u32 blocks) {
    Code code;
    for (u32 i = 0; i < blocks; ++i) {
        code[Entry + i * 8] = AddImm(0, 0, 1);
        code[Entry + i * 8 + 4] = B(4);
    }
    code[Entry + blocks * 8] = Svc0;
    return code;
}

class Environment final : public Dynarmic::A64::UserCallbacks {
public:
    explicit Environment(u64 memory_value_)
        : memory_value{memory_value_} {}

    Jit* jit{};
    std::shared_ptr<const Code> code;
    const u64 memory_value;
    std::atomic<u64> memory_reads{0};
    std::atomic<bool> svc{false};
    std::atomic<bool> unexpected{false};
    /// Set when this Environment is destroyed, so a test can prove the order: shared code must
    /// not use it after that.
    std::atomic<bool>* destroyed = nullptr;
    /// Blocks the JIT reported as compiled (UserCallbacks::BlockCompiled).
    std::atomic<u64> block_compiled{0};

    ~Environment() override {
        if (destroyed != nullptr) {
            destroyed->store(true);
        }
    }

    void BlockCompiled(u64) override {
        block_compiled.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<u32> MemoryReadCode(u64 address) override {
        const auto it = code->find(address);
        return it == code->end() ? std::nullopt : std::optional<u32>{it->second};
    }
    u64 MemoryRead(u64, size_t) override {
        memory_reads.fetch_add(1, std::memory_order_relaxed);
        return memory_value;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        unexpected = true;
        return {};
    }
    void MemoryWrite(u64, u64, size_t) override {
        unexpected = true;
    }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {
        unexpected = true;
    }
    void CallSVC(u32) override {
        svc = true;
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override {
        unexpected = true;
        jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

/// A core: its callbacks, TPIDR_EL0 and Jit, with the emulator's x86-64 options (no page table:
/// memory goes through the callbacks).
struct Core {
    Environment env;
    u64 tpidr_el0;
    std::optional<Jit> jit;

    Core(std::shared_ptr<SharedCodeCache> cache, std::shared_ptr<const Code> code, u8 index,
         u32 code_cache_size = 64u << 20, u32 rsb_size = 32,
         Dynarmic::OptimizationFlag optimizations = Dynarmic::all_safe_optimizations)
        : env{1000u * (index + 1u)}, tpidr_el0{10u * (index + 1u)} {
        env.code = std::move(code);
        Dynarmic::A64::UserConfig config;
        config.callbacks = &env;
        config.processor_id = index;
        config.tpidr_el0 = &tpidr_el0;
        config.enable_cycle_counting = false;
        config.code_cache_size = code_cache_size;
        config.compact_block_exits = true;
        config.far_code = true;
        config.fast_dispatch_table_bits = 16;
        config.fast_dispatch_set_associative = true;
        config.rsb_size = rsb_size;
        config.optimizations = optimizations;
        config.halt_check_on_back_edges = true;
        config.reuse_host_flags = true;
        config.host_call_return = true;
        config.spill_to_free_gpr = true;
        config.scratch_copy_to_free_reg = true;
        config.elide_redundant_zext = true;
        config.omit_block_frame = true;
        config.shared_code_cache = std::move(cache);
        jit.emplace(config);
        env.jit = &*jit;
        jit->SetFpcr(0);
    }

    /// Runs from Entry to the svc; another Jit's invalidation can end a Run early.
    u64 RunProgram() {
        jit->SetPC(Entry);
        jit->SetRegister(0, 0);
        env.svc = false;
        while (!env.svc && !env.unexpected) {
            jit->Run();
        }
        return jit->GetRegister(0);
    }

    u64 LoopResult(u32 iterations) const {
        return u64(iterations) * (tpidr_el0 + env.memory_value + 1);
    }
};

std::vector<std::unique_ptr<Core>> MakeCores(size_t count, std::shared_ptr<SharedCodeCache> cache,
                                             std::shared_ptr<const Code> code,
                                             u32 code_cache_size = 64u << 20,
                                             Dynarmic::OptimizationFlag optimizations = Dynarmic::all_safe_optimizations) {
    std::vector<std::unique_ptr<Core>> cores;
    for (size_t i = 0; i < count; ++i) {
        cores.push_back(std::make_unique<Core>(cache, code, u8(i), code_cache_size, 32, optimizations));
    }
    return cores;
}

} // namespace

TEST_CASE("Jits sharing a code cache compile a block once", "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>(true);
    const auto code = std::make_shared<const Code>(LoopProgram(3));
    const auto cores = MakeCores(2, cache, code);
    REQUIRE(cores[0]->jit->SharesCode());
    REQUIRE(cores[1]->jit->SharesCode());

    REQUIRE(cores[0]->jit->Precompile(Entry, 0) == PrecompileResult::Compiled);
    const size_t used = cores[0]->jit->CodeCacheUsed();
    REQUIRE(cores[1]->jit->Precompile(Entry, 0) == PrecompileResult::Present);
    REQUIRE(cores[1]->jit->CodeCacheUsed() == used);
    REQUIRE(cache->VerifyFailures() == 0);
}

TEST_CASE("Shared code uses the callbacks and registers of the Jit that runs it",
          "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>(true);
    const auto code = std::make_shared<const Code>(LoopProgram(50));
    const auto cores = MakeCores(4, cache, code);
    // Core 0 compiles everything; the others run its code.
    for (const auto& core : cores) {
        REQUIRE(core->RunProgram() == core->LoopResult(50));
        REQUIRE(core->env.memory_reads == 50);
        REQUIRE_FALSE(core->env.unexpected);
    }
    REQUIRE(cache->VerifyFailures() == 0);
}

TEST_CASE("A Jit whose config differs keeps a cache of its own", "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>();
    const auto code = std::make_shared<const Code>(LoopProgram(5));
    Core first{cache, code, 0};
    Core other{cache, code, 1, 64u << 20, 16};
    REQUIRE(first.jit->SharesCode());
    REQUIRE_FALSE(other.jit->SharesCode());
    REQUIRE(other.RunProgram() == other.LoopResult(5));
    REQUIRE(first.RunProgram() == first.LoopResult(5));
}

TEST_CASE("Jits run, compile and link shared code at the same time", "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>(true);
    const auto code = std::make_shared<const Code>(ChainProgram(3000));
    const auto cores = MakeCores(4, cache, code);
    std::vector<u64> results(cores.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < cores.size(); ++i) {
        threads.emplace_back([&, i] {
            u64 last = 0;
            for (int round = 0; round < 20; ++round) {
                last = cores[i]->RunProgram();
                if (last != 3000) {
                    break;
                }
            }
            results[i] = last;
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (size_t i = 0; i < cores.size(); ++i) {
        INFO("core " << i);
        REQUIRE(results[i] == 3000);
        REQUIRE_FALSE(cores[i]->env.unexpected);
    }
    REQUIRE(cache->VerifyFailures() == 0);
}

TEST_CASE("Clearing the shared code while other Jits run it", "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>();
    const auto code = std::make_shared<const Code>(LoopProgram(2000));
    const auto cores = MakeCores(3, cache, code);
    std::atomic<bool> done{false};
    std::vector<bool> ok(cores.size(), false);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < cores.size(); ++i) {
        threads.emplace_back([&, i] {
            bool all = true;
            for (int round = 0; round < 40; ++round) {
                all = all && cores[i]->RunProgram() == cores[i]->LoopResult(2000);
            }
            ok[i] = all;
        });
    }
    std::thread clearer([&] {
        while (!done) {
            cores[0]->jit->ClearCache();
            std::this_thread::yield();
        }
    });
    for (auto& thread : threads) {
        thread.join();
    }
    done = true;
    clearer.join();
    for (size_t i = 0; i < cores.size(); ++i) {
        INFO("core " << i);
        REQUIRE(ok[i]);
        REQUIRE_FALSE(cores[i]->env.unexpected);
    }
    REQUIRE(cache->Clears() > 0);
}

TEST_CASE("A full shared cache is cleared by the Jit that needs room", "[jit_shared_code]") {
    // 6 MiB with far code: 1.5 MiB of it cold, which the chain's exit stubs fill to under the
    // 1 MiB a compile needs left.
    const auto cache = std::make_shared<SharedCodeCache>();
    const auto code = std::make_shared<const Code>(ChainProgram(30000));
    const auto cores = MakeCores(2, cache, code, 6u << 20);
    std::vector<u64> results(cores.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < cores.size(); ++i) {
        threads.emplace_back([&, i] { results[i] = cores[i]->RunProgram(); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (size_t i = 0; i < cores.size(); ++i) {
        INFO("core " << i);
        REQUIRE(results[i] == 30000);
        REQUIRE_FALSE(cores[i]->env.unexpected);
    }
    REQUIRE(cache->Clears() > 0);
}

TEST_CASE("Invalidating a range recompiles it for every Jit sharing the code",
          "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>();
    auto program = LoopProgram(10);
    auto code = std::make_shared<const Code>(program);
    const auto cores = MakeCores(2, cache, code);
    REQUIRE(cores[0]->RunProgram() == cores[0]->LoopResult(10));
    REQUIRE(cores[1]->RunProgram() == cores[1]->LoopResult(10));

    // func now adds 2.
    program[Entry + 44] = AddImm(0, 0, 2);
    code = std::make_shared<const Code>(program);
    for (const auto& core : cores) {
        core->env.code = code;
    }
    cores[1]->jit->InvalidateCacheRange(Entry + 44, 4);
    for (const auto& core : cores) {
        REQUIRE(core->RunProgram() == core->LoopResult(10) + 10);
    }
}

TEST_CASE("A joined Jit registers every per-core slot its shared code reads",
          "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>();
    const auto code = std::make_shared<const Code>(LoopProgram(1));
    Core core{cache, code, 0};
    REQUIRE(core.jit->SharesCode());
    // Callbacks, JitImpl, UserConfig, TpidrEl0, TpidrroEl0 and the fast dispatch table. The
    // exclusive monitor slots are absent: this config has no global_monitor.
    REQUIRE(cache->RegisteredSlots() == 6);
}

TEST_CASE("A member Jit compiles after the founding Jit is destroyed", "[jit_shared_code]") {
    // The shared code must not keep the founding Jit's callbacks pointer: on an Itanium ABI
    // Devirtualize reads the object's vtable, so a member that compiles after that object is
    // destroyed would read freed memory. Only code addresses captured at join time survive.
    const auto cache = std::make_shared<SharedCodeCache>(true, true);
    const auto chain = std::make_shared<const Code>(ChainProgram(64));
    std::atomic<bool> founder_callbacks_destroyed{false};
    std::unique_ptr<Core> member;
    {
        Core founder{cache, chain, 0};
        founder.env.destroyed = &founder_callbacks_destroyed;
        REQUIRE(founder.jit->SharesCode());
        REQUIRE(founder.RunProgram() == 64);
        member = std::make_unique<Core>(cache, chain, 1);
        REQUIRE(member->jit->SharesCode());
        REQUIRE(member->RunProgram() == 64);
    }
    // The founder's callbacks object, UserConfig, TPIDR_EL0, fast dispatch table and Jit::Impl are
    // gone: Core destroys its Jit before its Environment, and the whole Core before this point.
    REQUIRE(founder_callbacks_destroyed.load());
    member->env.code = std::make_shared<const Code>(LoopProgram(7));
    member->jit->InvalidateCacheRange(Entry, 0x1000);
    const u64 compiles_before = cache->GetStats().compiles;
    REQUIRE(member->RunProgram() == member->LoopResult(7));
    REQUIRE_FALSE(member->env.unexpected);
    REQUIRE(cache->GetStats().compiles > compiles_before);
    REQUIRE(cache->VerifyFailures() == 0);

    // The counters the statistics were added for: a lookup per block dispatch, hits after the
    // first compile, and a bounded hold of compile_mutex.
    const auto stats = cache->GetStats();
    REQUIRE(stats.lookups > 0);
    REQUIRE(stats.hits > 0);
    REQUIRE(stats.compiles > 0);
    REQUIRE(stats.lock_hold_ns > 0);
}

// A shared-code site whose per-core slot was never registered fails in BlockOfCode::MovPerCore
// with ASSERT_MSG followed by UNREACHABLE_MSG. That aborts the process, which Catch2 cannot catch
// on Windows, so the failure path itself is not unit-tested; the invariant it protects (a joined
// Jit registers every slot the shared code emits) is covered above. The non-shared path is the
// plain literal emit, exercised by every existing test that uses a Jit without a shared cache.

TEST_CASE("BlockCompiled runs once per compiled block, not per lookup", "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>();
    const auto code = std::make_shared<const Code>(ChainProgram(8));
    const auto cores = MakeCores(2, cache, code);
    REQUIRE(cores[0]->RunProgram() == 8);
    const u64 compiled = cores[0]->env.block_compiled.load();
    REQUIRE(compiled > 0);
    for (int round = 0; round < 5; ++round) {
        REQUIRE(cores[0]->RunProgram() == 8);
        REQUIRE(cores[0]->env.block_compiled.load() == compiled);
    }
}

TEST_CASE("Invalidating while other Jits run the shared code", "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>();
    const auto code = std::make_shared<const Code>(ChainProgram(400));
    const auto cores = MakeCores(3, cache, code);
    std::atomic<bool> stop{false};
    std::vector<bool> ok(cores.size(), true);
    std::vector<std::thread> runners;
    for (size_t i = 0; i < cores.size(); ++i) {
        runners.emplace_back([&, i] {
            for (int round = 0; round < 25 && !stop.load(); ++round) {
                ok[i] = ok[i] && cores[i]->RunProgram() == 400;
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        cores[size_t(i) % cores.size()]->jit->InvalidateCacheRange(Entry, 0x1000);
        std::this_thread::yield();
    }
    stop = true;
    for (auto& runner : runners) {
        runner.join();
    }
    for (size_t i = 0; i < cores.size(); ++i) {
        INFO("core " << i);
        REQUIRE(ok[i]);
        REQUIRE_FALSE(cores[i]->env.unexpected);
    }
}

TEST_CASE("Members join and leave while the shared code is used", "[jit_shared_code]") {
    const auto cache = std::make_shared<SharedCodeCache>();
    const auto code = std::make_shared<const Code>(ChainProgram(200));
    Core resident{cache, code, 0};
    std::atomic<bool> stop{false};
    std::thread worker{[&] {
        while (!stop.load()) {
            resident.RunProgram();
            resident.jit->InvalidateCacheRange(Entry, 0x1000);
        }
    }};
    for (int i = 0; i < 20; ++i) {
        auto member = std::make_unique<Core>(cache, code, 1);
        REQUIRE(member->jit->SharesCode());
        REQUIRE(member->RunProgram() == 200);
        member.reset();
    }
    stop = true;
    worker.join();
    REQUIRE(cache->VerifyFailures() == 0);
}

// Run explicitly: tests.exe "[benchmark]". Each Jit runs one SVC block per Run, so each Run does
// one shared-code lookup; FastDispatch is off so the lookup cannot be served by that table. The
// locked mode is the pre-change path (every lookup takes compile_mutex).
TEST_CASE("Shared code lookup throughput", "[.][benchmark]") {
    const auto code = std::make_shared<const Code>(Code{{Entry, Svc0}});
    const auto optimizations =
        Dynarmic::all_safe_optimizations & ~Dynarmic::OptimizationFlag::FastDispatch;
    const auto measure = [&](std::shared_ptr<SharedCodeCache> cache, int threads, int iterations) {
        const auto cores = MakeCores(size_t(threads), cache, code, 64u << 20, optimizations);
        for (const auto& core : cores) {
            REQUIRE(core->RunProgram() == 0);
        }
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> runners;
        for (int t = 0; t < threads; ++t) {
            runners.emplace_back([&, t] {
                for (int i = 0; i < iterations; ++i) {
                    cores[size_t(t)]->RunProgram();
                }
            });
        }
        for (auto& runner : runners) {
            runner.join();
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        return double(ns) / (double(threads) * double(iterations));
    };
    for (int round = 0; round < 3; ++round) {
        const double own = measure(nullptr, 4, 50'000);
        const double locked = measure(std::make_shared<SharedCodeCache>(false, false, false), 4, 50'000);
        const double fast = measure(std::make_shared<SharedCodeCache>(false, false, true), 4, 50'000);
        std::printf("round %d: ns/lookup own=%.0f shared-locked=%.0f shared-fast=%.0f fast/locked=%.2fx\n",
                    round, own, locked, fast, locked / fast);
    }
    SUCCEED();
}

// Same A/B, but each Run walks 256 indirect-branch blocks: one shared-code lookup per block, so
// Run entry/exit is amortized and the number is the lookup path itself.
TEST_CASE("Shared code dispatcher throughput", "[.][benchmark]") {
    constexpr u32 Blocks = 256;
    const auto code = std::make_shared<const Code>(IndirectChainProgram(Blocks));
    const auto optimizations =
        Dynarmic::all_safe_optimizations & ~Dynarmic::OptimizationFlag::FastDispatch;
    const auto measure = [&](std::shared_ptr<SharedCodeCache> cache, int threads, int iterations) {
        const auto cores = MakeCores(size_t(threads), cache, code, 64u << 20, optimizations);
        for (const auto& core : cores) {
            REQUIRE(core->RunProgram() == 0);
        }
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> runners;
        for (int t = 0; t < threads; ++t) {
            runners.emplace_back([&, t] {
                for (int i = 0; i < iterations; ++i) {
                    cores[size_t(t)]->RunProgram();
                }
            });
        }
        for (auto& runner : runners) {
            runner.join();
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        return double(ns) / (double(threads) * double(iterations) * double(Blocks + 1));
    };
    for (int round = 0; round < 3; ++round) {
        const double own = measure(nullptr, 4, 4'000);
        const double locked = measure(std::make_shared<SharedCodeCache>(false, false, false), 4, 4'000);
        const double fast = measure(std::make_shared<SharedCodeCache>(false, false, true), 4, 4'000);
        std::printf("round %d: ns/lookup (256 blocks) own=%.1f shared-locked=%.1f shared-fast=%.1f fast/locked=%.2fx\n",
                    round, own, locked, fast, locked / fast);
    }
    SUCCEED();
}

#endif

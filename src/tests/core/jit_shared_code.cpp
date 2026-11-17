// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
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

using Code = std::map<u64, u32>;

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
         u32 code_cache_size = 64u << 20, u32 rsb_size = 32)
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
                                             u32 code_cache_size = 64u << 20) {
    std::vector<std::unique_ptr<Core>> cores;
    for (size_t i = 0; i < count; ++i) {
        cores.push_back(std::make_unique<Core>(cache, code, u8(i), code_cache_size));
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

#endif

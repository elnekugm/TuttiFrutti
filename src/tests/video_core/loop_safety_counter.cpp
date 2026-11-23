// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <deque>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
using NodeType = IR::AbstractSyntaxNode::Type;

class TestEnvironment final : public Shader::Environment {
public:
    TestEnvironment() { stage = Shader::Stage::Compute; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

/// i = 0; do { } while (++i < 1); twice in a row.
struct TwoLoops {
    Shader::ObjectPool<IR::Inst> pool{64};
    std::deque<IR::Block> blocks;
    IR::Program program;

    IR::Block& Block() {
        IR::Block& block{blocks.emplace_back(pool)};
        block.SetOrder(static_cast<u32>(blocks.size() - 1));
        return block;
    }

    size_t CountSpirv(spv::Op opcode) {
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        const std::vector<u32> code{Shader::Backend::SPIRV::EmitSPIRV(profile, program)};
        size_t count{};
        for (size_t offset = 5; offset < code.size(); offset += code[offset] >> 16) {
            REQUIRE((code[offset] >> 16) != 0);
            count += (code[offset] & 0xffffU) == static_cast<u32>(opcode);
        }
        return count;
    }

    TwoLoops() {
        IR::Block& entry{Block()};
        IR::Block& first_header{Block()};
        IR::Block& first_body{Block()};
        IR::Block& first_next{Block()};
        IR::Block& first_merge{Block()};
        IR::Block& second_header{Block()};
        IR::Block& second_body{Block()};
        IR::Block& second_next{Block()};
        IR::Block& second_merge{Block()};
        IR::Block& exit{Block()};

        entry.AddBranch(&first_header);
        first_header.AddBranch(&first_body);
        first_body.AddBranch(&first_next);
        first_next.AddBranch(&first_header);
        first_next.AddBranch(&first_merge);
        first_merge.AddBranch(&second_header);
        second_header.AddBranch(&second_body);
        second_body.AddBranch(&second_next);
        second_next.AddBranch(&second_header);
        second_next.AddBranch(&second_merge);
        second_merge.AddBranch(&exit);

        IR::IREmitter start{entry};
        first_header.AppendNewInst(IR::Opcode::Phi, {});
        IR::Inst* const first_index{&first_header.back()};
        first_index->SetFlags(IR::Type::U32);
        IR::IREmitter first_step{first_next};
        const IR::U32 first_in_c{first_step.IAdd(IR::U32{IR::Value{first_index}},
                                                 first_step.Imm32(1))};
        const IR::U1 first_cond{
            first_step.ConditionRef(first_step.ILessThan(first_in_c, first_step.Imm32(1), false))};
        first_index->AddPhiOperand(&entry, start.Imm32(0));
        first_index->AddPhiOperand(&first_next, first_in_c);

        IR::IREmitter in_merge{first_merge};
        second_header.AppendNewInst(IR::Opcode::Phi, {});
        IR::Inst* const second_index{&second_header.back()};
        second_index->SetFlags(IR::Type::U32);
        IR::IREmitter second_step{second_next};
        const IR::U32 second_in_c{second_step.IAdd(IR::U32{IR::Value{second_index}},
                                                  second_step.Imm32(1))};
        const IR::U1 second_cond{second_step.ConditionRef(
            second_step.ILessThan(second_in_c, second_step.Imm32(1), false))};
        second_index->AddPhiOperand(&first_merge, in_merge.Imm32(0));
        second_index->AddPhiOperand(&second_next, second_in_c);

        for (IR::Block& block : blocks) {
            program.blocks.push_back(&block);
        }
        program.stage = Shader::Stage::Compute;
        program.workgroup_size = {64, 1, 1};
        TestEnvironment env;
        Shader::Optimization::CollectShaderInfoPass(env, program);
        program.post_order_blocks = {&exit,         &second_merge, &second_next, &second_body,
                                     &second_header, &first_merge, &first_next,  &first_body,
                                     &first_header, &entry};
        program.syntax_list = {
            {.data{.block = &entry}, .type = NodeType::Block},
            {.data{.block = &first_header}, .type = NodeType::Block},
            {.data{.loop{&first_body, &first_next, &first_merge}}, .type = NodeType::Loop},
            {.data{.block = &first_body}, .type = NodeType::Block},
            {.data{.block = &first_next}, .type = NodeType::Block},
            {.data{.repeat{first_cond, &first_header, &first_merge}}, .type = NodeType::Repeat},
            {.data{.block = &first_merge}, .type = NodeType::Block},
            {.data{.block = &second_header}, .type = NodeType::Block},
            {.data{.loop{&second_body, &second_next, &second_merge}}, .type = NodeType::Loop},
            {.data{.block = &second_body}, .type = NodeType::Block},
            {.data{.block = &second_next}, .type = NodeType::Block},
            {.data{.repeat{second_cond, &second_header, &second_merge}}, .type = NodeType::Repeat},
            {.data{.block = &second_merge}, .type = NodeType::Block},
            {.type = NodeType::Return},
        };
    }
};

} // Anonymous namespace

TEST_CASE("A loop's safety counter is set again where the loop exits",
          "[shader_recompiler][loop_safety]") {
    // One store per loop decrements its counter, and one sets it back to the whole count as the
    // loop exits, so that the next entry of the loop starts with the whole count too. Without the
    // second one a loop entered again (one nested in another, on every outer iteration) ran out
    // mid-dispatch: Kirby Air Riders' list builders stopped early and the records they never wrote
    // kept vertex-like data its draws then read as counts, which lost the GPU.
    TwoLoops program;
    CHECK(program.CountSpirv(spv::Op::OpStore) == 4);
}
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/vm.hpp>
#include <cedar/vm/program_slot.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/opcodes/utility.hpp>
#include <cedar/dsp/constants.hpp>

#include <array>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

std::array<Instruction, 2> const_to_output_program(float value) {
    return {
        make_const_instruction(Opcode::PUSH_CONST, 0, value),
        Instruction::make_unary(Opcode::OUTPUT, 0, 0)
    };
}

}  // namespace

TEST_CASE("hot-swap: literal constant change propagates to audio", "[hot_swap]") {
    VM vm;
    vm.set_crossfade_blocks(3);

    auto bcA = const_to_output_program(0.5f);
    REQUIRE(vm.load_program(bcA) == VM::LoadResult::Success);

    std::array<float, BLOCK_SIZE> left{}, right{};

    // First block executes the swap (no crossfade — old slot was empty),
    // second block produces audio from program A.
    vm.process_block(left.data(), right.data());
    vm.process_block(left.data(), right.data());
    CHECK_THAT(left[0], WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(right[0], WithinAbs(0.5f, 1e-6f));

    auto bcB = const_to_output_program(0.7f);
    REQUIRE(vm.load_program(bcB) == VM::LoadResult::Success);

    // Run enough blocks to cover the swap and the full crossfade.
    for (int i = 0; i < 16; ++i) {
        vm.process_block(left.data(), right.data());
    }

    CHECK_THAT(left[0], WithinAbs(0.7f, 1e-6f));
    CHECK_THAT(right[0], WithinAbs(0.7f, 1e-6f));
}

TEST_CASE("hot-swap: dag_hash differs when only literal changes", "[hot_swap]") {
    auto bcA = const_to_output_program(0.5f);
    auto bcB = const_to_output_program(0.7f);

    ProgramSlot slotA;
    ProgramSlot slotB;
    REQUIRE(slotA.load(bcA));
    REQUIRE(slotB.load(bcB));

    CHECK(slotA.signature != slotB.signature);
    CHECK(slotA.signature.dag_hash != slotB.signature.dag_hash);
}

TEST_CASE("hot-swap: dag_hash matches for identical bytecode", "[hot_swap]") {
    auto bcA1 = const_to_output_program(0.5f);
    auto bcA2 = const_to_output_program(0.5f);

    ProgramSlot slotA1;
    ProgramSlot slotA2;
    REQUIRE(slotA1.load(bcA1));
    REQUIRE(slotA2.load(bcA2));

    CHECK(slotA1.signature == slotA2.signature);
}

TEST_CASE("hot-swap: rapid sequential loads all eventually apply", "[hot_swap]") {
    // The worklet's retry loop relies on load_program eventually succeeding
    // after enough audio blocks have advanced the crossfade. Verify the
    // load+process cycle reaches the final program even when many loads
    // happen close together.
    VM vm;
    vm.set_crossfade_blocks(3);

    std::array<float, BLOCK_SIZE> left{}, right{};

    constexpr int N = 20;
    for (int i = 0; i < N; ++i) {
        const float value = 0.1f + 0.01f * static_cast<float>(i);
        auto bc = const_to_output_program(value);

        // Drain blocks until a slot frees up. This mirrors what the worklet
        // does in process(): retry until LoadResult::Success.
        VM::LoadResult result = VM::LoadResult::SlotBusy;
        for (int retry = 0; retry < 32; ++retry) {
            result = vm.load_program(bc);
            if (result == VM::LoadResult::Success) break;
            vm.process_block(left.data(), right.data());
        }
        REQUIRE(result == VM::LoadResult::Success);

        vm.process_block(left.data(), right.data());
    }

    // Run enough blocks to fully complete the last crossfade.
    for (int i = 0; i < 16; ++i) {
        vm.process_block(left.data(), right.data());
    }

    const float expected = 0.1f + 0.01f * static_cast<float>(N - 1);
    CHECK_THAT(left[0], WithinAbs(expected, 1e-5f));
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/state_op.hpp"
#include "cedar/opcodes/utility.hpp"
#include "cedar/dsp/constants.hpp"
#include <array>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

Instruction make_state(std::uint8_t mode,
                       std::uint16_t out,
                       std::uint16_t in0 = BUFFER_UNUSED,
                       std::uint32_t state_id = 0xCAFEF00D) {
    Instruction inst{};
    inst.opcode = Opcode::STATE_OP;
    inst.rate = mode;
    inst.out_buffer = out;
    inst.inputs[0] = in0;
    inst.inputs[1] = BUFFER_UNUSED;
    inst.inputs[2] = BUFFER_UNUSED;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = state_id;
    return inst;
}

}  // namespace

TEST_CASE("STATE_OP rate=0 init writes value once", "[state_op][init]") {
    VM vm;

    Instruction init = make_state(/*mode*/ 0, /*out*/ 0, /*in*/ 1);
    vm.load_program(std::span{&init, 1});

    // First block: pre-fill input buffer with constant 7.5
    std::array<float, BLOCK_SIZE> in_buf{};
    std::fill(in_buf.begin(), in_buf.end(), 7.5f);
    std::copy(in_buf.begin(), in_buf.end(), vm.buffers().get(1));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* out = vm.buffers().get(0);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(out[i], WithinAbs(7.5f, 1e-6f));
    }

    // Second block: change the input value — init should NOT overwrite,
    // because state.initialized is already true.
    std::fill(in_buf.begin(), in_buf.end(), 99.0f);
    std::copy(in_buf.begin(), in_buf.end(), vm.buffers().get(1));
    vm.process_block(L.data(), R.data());

    out = vm.buffers().get(0);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(out[i], WithinAbs(7.5f, 1e-6f));
    }
}

TEST_CASE("STATE_OP rate=2 store uses last sample only", "[state_op][store]") {
    VM vm;

    // Init to 0, then store a varying signal — only the last sample should win.
    std::array<Instruction, 2> program = {
        make_state(/*mode*/ 0, /*out*/ 0, /*in*/ 1, /*state_id*/ 0xABCD),
        make_state(/*mode*/ 2, /*out*/ 2, /*in*/ 3, /*state_id*/ 0xABCD),
    };
    vm.load_program(std::span(program));

    // init buf 1 = 0.0
    std::array<float, BLOCK_SIZE> zeros{};
    std::copy(zeros.begin(), zeros.end(), vm.buffers().get(1));

    // store buf 3 = ramp; last sample = (BLOCK_SIZE-1)/BLOCK_SIZE = 127/128
    std::array<float, BLOCK_SIZE> ramp{};
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        ramp[i] = static_cast<float>(i) / static_cast<float>(BLOCK_SIZE);
    }
    std::copy(ramp.begin(), ramp.end(), vm.buffers().get(3));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // Output of store (buf 2) is broadcast of the new slot value (last sample).
    const float* store_out = vm.buffers().get(2);
    const float expected = ramp[BLOCK_SIZE - 1];
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(store_out[i], WithinAbs(expected, 1e-6f));
    }
}

TEST_CASE("STATE_OP load reads slot value", "[state_op][load]") {
    VM vm;

    // Block 1: init=42, then store a constant 5, then load.
    // Block 2: load again — should still see 5 (stored value persists).
    std::array<Instruction, 3> program = {
        make_state(/*mode*/ 0, /*out*/ 0, /*in*/ 1, /*state_id*/ 0x1234),
        make_state(/*mode*/ 2, /*out*/ 2, /*in*/ 3, /*state_id*/ 0x1234),
        make_state(/*mode*/ 1, /*out*/ 4, /*in*/ BUFFER_UNUSED, /*state_id*/ 0x1234),
    };
    vm.load_program(std::span(program));

    std::array<float, BLOCK_SIZE> init_buf{};
    std::fill(init_buf.begin(), init_buf.end(), 42.0f);
    std::copy(init_buf.begin(), init_buf.end(), vm.buffers().get(1));

    std::array<float, BLOCK_SIZE> store_buf{};
    std::fill(store_buf.begin(), store_buf.end(), 5.0f);
    std::copy(store_buf.begin(), store_buf.end(), vm.buffers().get(3));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* load_out = vm.buffers().get(4);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(load_out[i], WithinAbs(5.0f, 1e-6f));
    }

    // Block 2: zero the store-input so we can be sure the value is from the slot,
    // not from this block's input.
    std::fill(store_buf.begin(), store_buf.end(), 0.0f);
    std::copy(store_buf.begin(), store_buf.end(), vm.buffers().get(3));
    vm.process_block(L.data(), R.data());

    // After block 2, the slot was overwritten with 0 (the store ran).
    load_out = vm.buffers().get(4);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(load_out[i], WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("STATE_OP distinct state_ids have independent slots", "[state_op][slots]") {
    VM vm;

    // Two cells, two different state_ids. Init each to a different value
    // and load both — they must not interfere.
    std::array<Instruction, 4> program = {
        make_state(/*mode*/ 0, /*out*/ 0, /*in*/ 1, /*state_id*/ 0xAAAA),
        make_state(/*mode*/ 0, /*out*/ 2, /*in*/ 3, /*state_id*/ 0xBBBB),
        make_state(/*mode*/ 1, /*out*/ 4, /*in*/ BUFFER_UNUSED, /*state_id*/ 0xAAAA),
        make_state(/*mode*/ 1, /*out*/ 5, /*in*/ BUFFER_UNUSED, /*state_id*/ 0xBBBB),
    };
    vm.load_program(std::span(program));

    std::array<float, BLOCK_SIZE> a{}, b{};
    std::fill(a.begin(), a.end(), 11.0f);
    std::fill(b.begin(), b.end(), 22.0f);
    std::copy(a.begin(), a.end(), vm.buffers().get(1));
    std::copy(b.begin(), b.end(), vm.buffers().get(3));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    CHECK_THAT(vm.buffers().get(4)[0], WithinAbs(11.0f, 1e-6f));
    CHECK_THAT(vm.buffers().get(5)[0], WithinAbs(22.0f, 1e-6f));
}

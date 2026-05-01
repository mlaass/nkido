#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/edge_op.hpp"
#include "cedar/opcodes/utility.hpp"
#include "cedar/dsp/constants.hpp"
#include <array>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

// Build an EDGE_OP instruction with the requested mode and inputs.
Instruction make_edge(std::uint8_t mode,
                      std::uint16_t out,
                      std::uint16_t in0,
                      std::uint16_t in1 = BUFFER_UNUSED,
                      std::uint16_t in2 = BUFFER_UNUSED,
                      std::uint32_t state_id = 12345) {
    Instruction inst{};
    inst.opcode = Opcode::EDGE_OP;
    inst.rate = mode;
    inst.out_buffer = out;
    inst.inputs[0] = in0;
    inst.inputs[1] = in1;
    inst.inputs[2] = in2;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = state_id;
    return inst;
}

}  // namespace

TEST_CASE("EDGE_OP rate=0 sample-and-hold matches old SAH behaviour", "[edge_op][sah]") {
    VM vm;

    // buf 0 = ramp 0..1, buf 1 = trigger pulse at sample 32
    std::array<float, BLOCK_SIZE> ramp{}, trig{};
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        ramp[i] = static_cast<float>(i) / static_cast<float>(BLOCK_SIZE - 1);
        trig[i] = (i == 32) ? 1.0f : 0.0f;
    }

    // Plan: load ramp/trig via PUSH_CONST is not enough (per-sample), so instead
    // we'll write into the buffer pool directly and execute the EDGE_OP alone.
    Instruction edge = make_edge(/*mode*/ 0, /*out*/ 0, /*in*/ 1, /*trig*/ 2);
    vm.load_program(std::span{&edge, 1});

    // Pre-fill input buffers (after load so the buffer pool is initialized)
    std::copy(ramp.begin(), ramp.end(), vm.buffers().get(1));
    std::copy(trig.begin(), trig.end(), vm.buffers().get(2));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* held = vm.buffers().get(0);
    // Before the trigger: held = 0.0 (initial)
    CHECK_THAT(held[0], WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(held[31], WithinAbs(0.0f, 1e-6f));
    // From sample 32 onward: held = ramp[32]
    CHECK_THAT(held[32], WithinAbs(ramp[32], 1e-6f));
    CHECK_THAT(held[64], WithinAbs(ramp[32], 1e-6f));
    CHECK_THAT(held[BLOCK_SIZE - 1], WithinAbs(ramp[32], 1e-6f));
}

TEST_CASE("EDGE_OP rate=1 gateup pulses on rising edges", "[edge_op][gateup]") {
    VM vm;

    // sig: 0,0,1,1,1,0,0,1,1,0,...  → rising edges at samples 2 and 7
    std::array<float, BLOCK_SIZE> sig{};
    sig[2] = sig[3] = sig[4] = 1.0f;
    sig[7] = sig[8] = 1.0f;

    Instruction edge = make_edge(/*mode*/ 1, /*out*/ 0, /*sig*/ 1);
    vm.load_program(std::span{&edge, 1});
    std::copy(sig.begin(), sig.end(), vm.buffers().get(1));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* out = vm.buffers().get(0);
    int pulses = 0;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (out[i] > 0.5f) ++pulses;
    }
    CHECK(pulses == 2);
    CHECK_THAT(out[2], WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(out[7], WithinAbs(1.0f, 1e-6f));
    // No pulse on falling edges
    CHECK_THAT(out[5], WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(out[9], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("EDGE_OP rate=2 gatedown pulses on falling edges", "[edge_op][gatedown]") {
    VM vm;

    // sig rises at 2, falls at 5; rises at 7, falls at 9.
    std::array<float, BLOCK_SIZE> sig{};
    sig[2] = sig[3] = sig[4] = 1.0f;
    sig[7] = sig[8] = 1.0f;

    Instruction edge = make_edge(/*mode*/ 2, /*out*/ 0, /*sig*/ 1);
    vm.load_program(std::span{&edge, 1});
    std::copy(sig.begin(), sig.end(), vm.buffers().get(1));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* out = vm.buffers().get(0);
    int pulses = 0;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (out[i] > 0.5f) ++pulses;
    }
    CHECK(pulses == 2);
    CHECK_THAT(out[5], WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(out[9], WithinAbs(1.0f, 1e-6f));
    // No pulse on rising edges
    CHECK_THAT(out[2], WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(out[7], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("EDGE_OP rate=3 counter increments and resets", "[edge_op][counter]") {
    VM vm;

    SECTION("bare counter increments on each rising edge") {
        std::array<float, BLOCK_SIZE> trig{};
        // Rising edges at samples 5, 20, 50, 100 → counter ends at 4
        trig[5] = trig[6] = 1.0f;
        trig[20] = trig[21] = 1.0f;
        trig[50] = trig[51] = 1.0f;
        trig[100] = trig[101] = 1.0f;

        Instruction edge = make_edge(/*mode*/ 3, /*out*/ 0, /*trig*/ 1,
                                     BUFFER_UNUSED, BUFFER_UNUSED);
        vm.load_program(std::span{&edge, 1});
        std::copy(trig.begin(), trig.end(), vm.buffers().get(1));

        std::array<float, BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());

        const float* out = vm.buffers().get(0);
        CHECK_THAT(out[0], WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(out[4], WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(out[5], WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(out[19], WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(out[20], WithinAbs(2.0f, 1e-6f));
        CHECK_THAT(out[50], WithinAbs(3.0f, 1e-6f));
        CHECK_THAT(out[100], WithinAbs(4.0f, 1e-6f));
        CHECK_THAT(out[BLOCK_SIZE - 1], WithinAbs(4.0f, 1e-6f));
    }

    SECTION("reset returns counter to 0 on rising edge of reset input") {
        std::array<float, BLOCK_SIZE> trig{}, reset{};
        trig[5] = trig[6] = 1.0f;
        trig[10] = trig[11] = 1.0f;
        reset[20] = reset[21] = 1.0f;
        trig[40] = trig[41] = 1.0f;

        Instruction edge = make_edge(/*mode*/ 3, /*out*/ 0, /*trig*/ 1,
                                     /*reset*/ 2, BUFFER_UNUSED);
        vm.load_program(std::span{&edge, 1});
        std::copy(trig.begin(), trig.end(), vm.buffers().get(1));
        std::copy(reset.begin(), reset.end(), vm.buffers().get(2));

        std::array<float, BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());

        const float* out = vm.buffers().get(0);
        CHECK_THAT(out[19], WithinAbs(2.0f, 1e-6f));
        CHECK_THAT(out[20], WithinAbs(0.0f, 1e-6f));  // reset
        CHECK_THAT(out[40], WithinAbs(1.0f, 1e-6f));  // count resumes from 0
    }

    SECTION("reset wins when trig and reset fire on the same sample") {
        std::array<float, BLOCK_SIZE> trig{}, reset{};
        // Bump count to 1 first.
        trig[5] = trig[6] = 1.0f;
        // Both fire on sample 20 — reset must win, count stays at 0 (no +1 from trig).
        trig[20] = trig[21] = 1.0f;
        reset[20] = reset[21] = 1.0f;

        Instruction edge = make_edge(/*mode*/ 3, /*out*/ 0, /*trig*/ 1,
                                     /*reset*/ 2, BUFFER_UNUSED);
        vm.load_program(std::span{&edge, 1});
        std::copy(trig.begin(), trig.end(), vm.buffers().get(1));
        std::copy(reset.begin(), reset.end(), vm.buffers().get(2));

        std::array<float, BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());

        const float* out = vm.buffers().get(0);
        CHECK_THAT(out[19], WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(out[20], WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(out[BLOCK_SIZE - 1], WithinAbs(0.0f, 1e-6f));
    }

    SECTION("start input sets reset target") {
        std::array<float, BLOCK_SIZE> trig{}, reset{}, start{};
        trig[5] = trig[6] = 1.0f;        // count=1
        reset[20] = reset[21] = 1.0f;    // reset to start[20]
        std::fill(start.begin(), start.end(), 7.0f);

        Instruction edge = make_edge(/*mode*/ 3, /*out*/ 0, /*trig*/ 1,
                                     /*reset*/ 2, /*start*/ 3);
        vm.load_program(std::span{&edge, 1});
        std::copy(trig.begin(), trig.end(), vm.buffers().get(1));
        std::copy(reset.begin(), reset.end(), vm.buffers().get(2));
        std::copy(start.begin(), start.end(), vm.buffers().get(3));

        std::array<float, BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());

        const float* out = vm.buffers().get(0);
        CHECK_THAT(out[19], WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(out[20], WithinAbs(7.0f, 1e-6f));
    }

    SECTION("counter persists across blocks") {
        std::array<float, BLOCK_SIZE> trig{};
        trig[5] = trig[6] = 1.0f;
        trig[40] = trig[41] = 1.0f;

        Instruction edge = make_edge(/*mode*/ 3, /*out*/ 0, /*trig*/ 1,
                                     BUFFER_UNUSED, BUFFER_UNUSED);
        vm.load_program(std::span{&edge, 1});
        std::copy(trig.begin(), trig.end(), vm.buffers().get(1));

        std::array<float, BLOCK_SIZE> L{}, R{};
        vm.process_block(L.data(), R.data());
        // After block 1 the counter holds 2.

        // Block 2: zero trig, value should remain at 2.
        std::array<float, BLOCK_SIZE> zero{};
        std::copy(zero.begin(), zero.end(), vm.buffers().get(1));
        vm.process_block(L.data(), R.data());

        const float* out = vm.buffers().get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(out[i], WithinAbs(2.0f, 1e-6f));
        }
    }
}

// PRD §9 edge case: `gateup(constant)` where constant is non-zero. The first
// block sees a 0 → constant rising edge at sample 0 because prev_trigger
// initializes to 0; subsequent samples and blocks output 0 because there is
// no further transition.
TEST_CASE("EDGE_OP gateup of a non-zero constant pulses once on first sample",
          "[edge_op][gateup][edge_case]") {
    VM vm;
    std::array<float, BLOCK_SIZE> sig{};
    std::fill(sig.begin(), sig.end(), 1.0f);

    Instruction edge = make_edge(/*mode*/ 1, /*out*/ 0, /*sig*/ 1);
    vm.load_program(std::span{&edge, 1});
    std::copy(sig.begin(), sig.end(), vm.buffers().get(1));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* out = vm.buffers().get(0);
    CHECK_THAT(out[0], WithinAbs(1.0f, 1e-6f));
    for (std::size_t i = 1; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(out[i], WithinAbs(0.0f, 1e-6f));
    }

    // Block 2: constant input still 1.0 → no edge → all zeros.
    std::copy(sig.begin(), sig.end(), vm.buffers().get(1));
    vm.process_block(L.data(), R.data());
    out = vm.buffers().get(0);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(out[i], WithinAbs(0.0f, 1e-6f));
    }
}

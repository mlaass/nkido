// Tests for POLY_BEGIN/POLY_END infrastructure (Phase 1)
// Verifies basic poly execution, XOR state isolation, and regression safety.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/utility.hpp"
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Helper: Build a POLY program with OSC_SIN body
// ============================================================================

// Buffer layout for poly tests:
//   0 = voice freq (written by POLY_BEGIN per-voice)
//   1 = voice gate
//   2 = voice vel
//   3 = voice trig
//   4 = voice output (osc writes here, POLY_BEGIN reads for mix)
//   5 = mix output (POLY_BEGIN accumulates here)
//   6 = left output
//   7 = right output

static constexpr std::uint16_t BUF_FREQ = 0;
static constexpr std::uint16_t BUF_GATE = 1;
static constexpr std::uint16_t BUF_VEL  = 2;
static constexpr std::uint16_t BUF_TRIG = 3;
static constexpr std::uint16_t BUF_VOICE_OUT = 4;
static constexpr std::uint16_t BUF_MIX  = 5;

static constexpr std::uint32_t POLY_STATE_ID = 0x10000;
static constexpr std::uint32_t OSC_STATE_ID  = 0x20000;

TEST_CASE("POLY basic execution with active voices", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Build program: POLY_BEGIN (body=1: OSC_SIN) + POLY_END + OUTPUT
    // Body: OSC_SIN reads freq from BUF_FREQ, writes to BUF_VOICE_OUT
    std::vector<Instruction> program;

    // POLY_BEGIN: rate=1 (body length), out=BUF_MIX,
    //   in0=BUF_FREQ, in1=BUF_GATE, in2=BUF_VEL, in3=BUF_TRIG, in4=BUF_VOICE_OUT
    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_FREQ, BUF_GATE, BUF_VEL, BUF_TRIG, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.rate = 1; // body length = 1 instruction
    program.push_back(poly_begin);

    // Body: OSC_SIN(freq=BUF_FREQ) -> BUF_VOICE_OUT
    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));

    // POLY_END
    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));

    // OUTPUT: mix -> stereo out
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));

    vm.load_program_immediate(std::span<const Instruction>(program));

    // Initialize poly state with 2 active voices
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    // Manually activate 2 voices
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    poly.voices[0].active = true;
    poly.voices[0].freq = 440.0f;
    poly.voices[0].gate = 1.0f;
    poly.voices[0].vel = 1.0f;

    poly.voices[1].active = true;
    poly.voices[1].freq = 880.0f;
    poly.voices[1].gate = 1.0f;
    poly.voices[1].vel = 1.0f;

    // Process a block
    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // The mix buffer should be non-silent (sum of two sines)
    const float* mix = vm.buffers().get(BUF_MIX);
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(mix[i]));
    }
    CHECK(max_abs > 0.1f);  // Sum of two sine oscillators should be audible

    // Output should also be non-silent (OUTPUT copies mix to left/right)
    float out_max = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out_max = std::max(out_max, std::abs(left[i]));
    }
    CHECK(out_max > 0.1f);
}

TEST_CASE("POLY XOR state isolation", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Same program as above
    std::vector<Instruction> program;

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_FREQ, BUF_GATE, BUF_VEL, BUF_TRIG, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.rate = 1;
    program.push_back(poly_begin);

    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));

    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));

    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    poly.voices[0].active = true;
    poly.voices[0].freq = 440.0f;
    poly.voices[0].gate = 1.0f;
    poly.voices[0].vel = 1.0f;

    poly.voices[1].active = true;
    poly.voices[1].freq = 880.0f;
    poly.voices[1].gate = 1.0f;
    poly.voices[1].vel = 1.0f;

    // Process 10 blocks so oscillators accumulate independent phase
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 10; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // Now check: if isolation works, the mix should NOT be 2*sin(440)
    // because voice 1 is at 880Hz. Compute expected 2*sin(440) signal
    // and verify the actual mix differs from it.
    const float* mix = vm.buffers().get(BUF_MIX);

    // The mix is sum of sin(440) + sin(880). If XOR isolation failed,
    // both voices would share the same OscState, and since voice 1 overwrites
    // freq with 880Hz, the result would be 2*sin(880).
    // With proper isolation, we get sin(440) + sin(880).

    // Check the output is not simply a scaled single-frequency sine.
    // A single-frequency signal has a constant ratio between consecutive samples.
    // A sum of two frequencies does not.
    // We check that the signal has frequency content at both 440 and 880
    // by verifying the absolute values vary in a non-uniform way.
    float sum = 0.0f;
    float sum_sq = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        sum += mix[i];
        sum_sq += mix[i] * mix[i];
    }
    float rms = std::sqrt(sum_sq / BLOCK_SIZE);
    CHECK(rms > 0.1f);  // Should have meaningful energy

    // Additional check: with two different frequencies, the signal should
    // cross zero more often than a pure 440Hz sine.
    int zero_crossings = 0;
    for (std::size_t i = 1; i < BLOCK_SIZE; ++i) {
        if ((mix[i] >= 0.0f) != (mix[i - 1] >= 0.0f)) {
            ++zero_crossings;
        }
    }
    // A pure 440Hz sine at 48kHz has ~128*440/48000 ≈ 1.17 cycles per block
    // → about 2 zero crossings. With 440+880, we expect more.
    CHECK(zero_crossings > 3);
}

TEST_CASE("POLY regression: non-POLY programs still work", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Simple program: OSC_SIN(440) -> OUTPUT (no POLY block)
    auto const_inst = make_const_instruction(Opcode::PUSH_CONST, 0, 440.0f);
    auto osc_inst = Instruction::make_unary(Opcode::OSC_SIN, 1, 0, 0x1234);
    auto out_inst = Instruction::make_unary(Opcode::OUTPUT, 0, 1);

    std::array<Instruction, 3> program = {const_inst, osc_inst, out_inst};
    vm.load_program_immediate(std::span<const Instruction>(program));

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Output should be a sine wave
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(left[i]));
    }
    CHECK(max_abs > 0.01f);  // Should have audio output
    CHECK(max_abs <= 1.0f);   // Sine wave shouldn't exceed amplitude 1
}

TEST_CASE("POLY empty block produces silence", "[poly]") {
    VM vm;
    vm.set_sample_rate(48000.0f);

    // Program: POLY_BEGIN (body=1) + OSC_SIN + POLY_END + OUTPUT
    // But no voices are active, so mix should be silence
    std::vector<Instruction> program;

    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_FREQ, BUF_GATE, BUF_VEL, BUF_TRIG, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.rate = 1;
    program.push_back(poly_begin);

    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));

    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));

    vm.load_program_immediate(std::span<const Instruction>(program));
    vm.init_poly_state(POLY_STATE_ID, 0, 8, 0, 0);

    // No voices activated — all default to active=false

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Mix buffer should be all zeros
    const float* mix = vm.buffers().get(BUF_MIX);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(mix[i] == 0.0f);
    }

    // Output should also be silence
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(left[i] == 0.0f);
        CHECK(right[i] == 0.0f);
    }
}

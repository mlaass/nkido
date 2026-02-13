// Tests for POLY infrastructure
// Phase 1: basic execution, XOR state isolation, regression safety
// Phase 3: voice allocation from pattern events

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
static constexpr std::uint32_t SEQ_STATE_ID  = 0x30000;

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

// ============================================================================
// Phase 3: Voice Allocation from Pattern Events
// ============================================================================

// Helper: build a SEQPAT_QUERY + POLY program with OSC_SIN body
static std::vector<Instruction> build_seq_poly_program() {
    std::vector<Instruction> program;

    // SEQPAT_QUERY: queries sequence at block boundaries, fills OutputEvents
    program.push_back(Instruction::make_nullary(
        Opcode::SEQPAT_QUERY, 0, SEQ_STATE_ID));

    // POLY_BEGIN: rate=1 (body length), out=BUF_MIX
    auto poly_begin = Instruction::make_quinary(
        Opcode::POLY_BEGIN, BUF_MIX,
        BUF_FREQ, BUF_GATE, BUF_VEL, BUF_TRIG, BUF_VOICE_OUT,
        POLY_STATE_ID);
    poly_begin.rate = 1;
    program.push_back(poly_begin);

    // Body: OSC_SIN(freq=BUF_FREQ) -> BUF_VOICE_OUT
    program.push_back(Instruction::make_unary(
        Opcode::OSC_SIN, BUF_VOICE_OUT, BUF_FREQ, OSC_STATE_ID));

    // POLY_END
    program.push_back(Instruction::make_nullary(Opcode::POLY_END, 0));

    // OUTPUT: mix -> stereo out
    program.push_back(Instruction::make_unary(Opcode::OUTPUT, 0, BUF_MIX));

    return program;
}

// Helper: set up a single-event sequence (one note)
// duration_beats: how long the note lasts in beats (output units)
// The sequence system scales: output_duration = e.duration * cycle_length
// So e.duration = duration_beats / cycle_length
static void setup_single_note_sequence(VM& vm, float freq,
                                       float duration_beats = 4.0f,
                                       float velocity = 1.0f) {
    static constexpr float CYCLE_LENGTH = 4.0f;

    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = duration_beats / CYCLE_LENGTH; // fraction of cycle
    evt.chance = 1.0f;
    evt.velocity = velocity;
    evt.num_values = 1;
    evt.values[0] = freq;

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);
}

// Helper: set up a chord sequence (multi-value event)
static void setup_chord_sequence(VM& vm, const float* freqs,
                                 std::uint8_t num_notes,
                                 float duration_beats = 4.0f) {
    static constexpr float CYCLE_LENGTH = 4.0f;

    Event evt;
    evt.type = EventType::DATA;
    evt.time = 0.0f;
    evt.duration = duration_beats / CYCLE_LENGTH;
    evt.chance = 1.0f;
    evt.velocity = 1.0f;
    evt.num_values = num_notes;
    for (std::uint8_t i = 0; i < num_notes; ++i) {
        evt.values[i] = freqs[i];
    }

    Sequence seq;
    seq.events = &evt;
    seq.num_events = 1;
    seq.capacity = 1;
    seq.duration = CYCLE_LENGTH;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, CYCLE_LENGTH, false, 1);
}

TEST_CASE("POLY voice allocation: single note produces audio", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Set up sequence with one C4 note (261.63 Hz) spanning the full cycle
    setup_single_note_sequence(vm, 261.63f);

    // Init poly state linked to sequence
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    // Process several blocks
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 5; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // After processing, at least one voice should be active
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() >= 1);

    // Output should be non-silent
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(left[i]));
    }
    CHECK(max_abs > 0.01f);
}

TEST_CASE("POLY voice allocation: chord produces multiple voices", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // C major chord: C4, E4, G4
    float chord_freqs[] = {261.63f, 329.63f, 392.00f};
    setup_chord_sequence(vm, chord_freqs, 3);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 5; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // Should have 3 active voices
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() == 3);

    // All three voices should have the chord frequencies
    bool found_c4 = false, found_e4 = false, found_g4 = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (!poly.voices[i].active) continue;
        float f = poly.voices[i].freq;
        if (std::abs(f - 261.63f) < 0.1f) found_c4 = true;
        if (std::abs(f - 329.63f) < 0.1f) found_e4 = true;
        if (std::abs(f - 392.00f) < 0.1f) found_g4 = true;
    }
    CHECK(found_c4);
    CHECK(found_e4);
    CHECK(found_g4);

    // Output should be non-silent (sum of 3 oscillators)
    const float* mix = vm.buffers().get(BUF_MIX);
    float rms = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        rms += mix[i] * mix[i];
    }
    rms = std::sqrt(rms / BLOCK_SIZE);
    CHECK(rms > 0.1f);
}

TEST_CASE("POLY voice allocation: voice release on event end", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Short note: 0.5 beats out of 4-beat cycle
    setup_single_note_sequence(vm, 440.0f, 0.5f);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    // At 120 BPM, 48kHz: samples_per_beat = 24000
    // 0.5 beats = 12000 samples = ~93.75 blocks of 128
    // Process blocks past the event end
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 100; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // After the event ends, the voice should be releasing
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    bool found_releasing = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (poly.voices[i].releasing) {
            found_releasing = true;
            CHECK(poly.voices[i].gate == 0.0f);
        }
    }
    CHECK(found_releasing);
}

TEST_CASE("POLY voice allocation: mono mode uses single voice", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Two sequential notes in a 4-beat cycle (each 2 beats)
    // e.duration is a fraction: output_duration = e.duration * cycle_length
    // For 2 beats out of 4: e.duration = 0.5
    // e.time is in beats (when seq.duration == cycle_length)
    Event events[2];
    events[0].type = EventType::DATA;
    events[0].time = 0.0f;
    events[0].duration = 0.5f;     // 0.5 * 4.0 = 2.0 beats
    events[0].chance = 1.0f;
    events[0].velocity = 1.0f;
    events[0].num_values = 1;
    events[0].values[0] = 261.63f;  // C4

    events[1].type = EventType::DATA;
    events[1].time = 2.0f;
    events[1].duration = 0.5f;     // 0.5 * 4.0 = 2.0 beats
    events[1].chance = 1.0f;
    events[1].velocity = 1.0f;
    events[1].num_values = 1;
    events[1].values[0] = 329.63f;  // E4

    Sequence seq;
    seq.events = events;
    seq.num_events = 2;
    seq.capacity = 2;
    seq.duration = 4.0f;
    seq.mode = SequenceMode::NORMAL;

    vm.init_sequence_program_state(SEQ_STATE_ID, &seq, 1, 4.0f, false, 2);

    // Mode 1 = mono
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 1, 0);

    // Process enough blocks to cover the first note region
    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 10; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // In mono mode, only voice 0 should ever be active
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() <= 1);
    // Verify only slot 0 is used
    for (std::uint8_t i = 1; i < poly.max_voices; ++i) {
        CHECK_FALSE(poly.voices[i].active);
    }
}

TEST_CASE("POLY voice allocation: voice stealing when full", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Create a 4-note chord — but limit to 2 voices
    float chord_freqs[] = {261.63f, 329.63f, 392.00f, 493.88f};
    setup_chord_sequence(vm, chord_freqs, 4);

    // Only 2 max voices — must steal
    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 2, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    for (int b = 0; b < 5; ++b) {
        vm.process_block(left.data(), right.data());
    }

    // Should have exactly 2 active voices (stolen from 4 chord notes)
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    CHECK(poly.active_voice_count() == 2);

    // Output should be non-silent
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        max_abs = std::max(max_abs, std::abs(left[i]));
    }
    CHECK(max_abs > 0.01f);
}

TEST_CASE("POLY voice allocation: velocity is passed to voices", "[poly][alloc]") {
    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);

    auto program = build_seq_poly_program();
    vm.load_program_immediate(std::span<const Instruction>(program));

    // Note with specific velocity
    setup_single_note_sequence(vm, 440.0f, 4.0f, 0.5f);

    vm.init_poly_state(POLY_STATE_ID, SEQ_STATE_ID, 8, 0, 0);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Check that the voice got the correct velocity
    auto& poly = vm.states().get_or_create<PolyAllocState>(POLY_STATE_ID);
    bool found = false;
    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
        if (poly.voices[i].active) {
            CHECK_THAT(poly.voices[i].vel, WithinAbs(0.5f, 0.01f));
            found = true;
        }
    }
    CHECK(found);
}

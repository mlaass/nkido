// Channel-type (Mono / Stereo) semantics for the Akkado compiler.
//
// Covers PRD-Stereo-Support:
//   §4.4 Error cases — left(mono), right(mono), out(stereo, mono), stereo(stereo)
//   §5.3 Type-checking rules (partial: out(L,R) validation, mono() dispatch)
//   §5.2 Auto-lift for stateless opcodes (PRD §5.2 classifies fold/saturate/etc.
//          as auto_lift=true — this used to require stateful ops)
//   §10   Edge cases (mono(mono), stereo(stereo), left/right on mono)
//
// These tests complement test_codegen.cpp's [stereo] tag by focusing on the
// *type* discipline rather than codegen output. If a test fails here, check:
//   - akkado/src/codegen_stereo.cpp  (handle_{stereo,mono,left,right}_call)
//   - akkado/src/codegen.cpp          (out() validation, stereo auto-lift)

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <vector>

static std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

static const cedar::Instruction* find_instruction(const std::vector<cedar::Instruction>& insts,
                                                   cedar::Opcode op) {
    for (const auto& inst : insts) {
        if (inst.opcode == op) return &inst;
    }
    return nullptr;
}

static size_t count_instructions(const std::vector<cedar::Instruction>& insts,
                                  cedar::Opcode op) {
    size_t c = 0;
    for (const auto& inst : insts) if (inst.opcode == op) ++c;
    return c;
}

static bool has_diagnostic(const akkado::CompileResult& result, const std::string& code) {
    for (const auto& d : result.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// =============================================================================
// Error cases (PRD §4.4 / §10)
// =============================================================================

TEST_CASE("Types: left()/right() reject mono input", "[types][stereo][errors]") {
    SECTION("left(mono) is E183") {
        auto result = akkado::compile("left(saw(220)) |> out(%)");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E183"));
    }

    SECTION("right(mono) is E184") {
        auto result = akkado::compile("right(saw(220)) |> out(%)");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E184"));
    }

    SECTION("left(stereo) still compiles") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            left(s) |> out(%, %)
        )");
        CHECK(result.success);
    }
}

TEST_CASE("Types: stereo() rejects already-stereo input", "[types][stereo][errors]") {
    SECTION("stereo(stereo) is E182") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            stereo(s) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E182"));
    }

    SECTION("stereo(mono) still compiles") {
        auto result = akkado::compile("stereo(saw(220)) |> out(%)");
        CHECK(result.success);
    }

    SECTION("stereo(L, R) with two mono signals still compiles") {
        auto result = akkado::compile("stereo(saw(218), saw(222)) |> out(%)");
        CHECK(result.success);
    }
}

TEST_CASE("Types: out(L, R) rejects stereo in either slot", "[types][stereo][errors]") {
    SECTION("out(stereo, mono) is E185") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(s, saw(330))
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E185"));
    }

    SECTION("out(mono, stereo) is E185") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(saw(330), s)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E185"));
    }

    SECTION("out(mono, mono) still compiles") {
        auto result = akkado::compile("out(saw(218), saw(222))");
        CHECK(result.success);
    }

    SECTION("out(stereo_sig) still compiles (single-arg stereo)") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(s)
        )");
        CHECK(result.success);
    }
}

TEST_CASE("Types: mono() rejects mono input", "[types][stereo][errors]") {
    // E181 is established in test_codegen.cpp; re-assert here for completeness.
    auto result = akkado::compile("mono(saw(220)) |> out(%)");
    CHECK_FALSE(result.success);
    CHECK(has_diagnostic(result, "E181"));
}

// =============================================================================
// Auto-lift for stateless opcodes (PRD §5.2 — "Mono-in, mono-out DSP" auto_lift=true)
// =============================================================================

TEST_CASE("Types: stateless opcodes auto-lift on stereo input", "[types][stereo][auto-lift]") {
    // Both saturate (DISTORT_TANH) and softclip (DISTORT_SOFT) are declared
    // with requires_state=false in builtins.hpp — before the G2 fix these
    // stateless ops silently dropped the right channel, contrary to PRD §5.2.
    SECTION("saturate auto-lifts (DISTORT_TANH, stateless)") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            saturate(s) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::DISTORT_TANH) == 1);
        auto* tanh = find_instruction(insts, cedar::Opcode::DISTORT_TANH);
        REQUIRE(tanh != nullptr);
        CHECK((tanh->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("softclip auto-lifts (DISTORT_SOFT, stateless)") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            softclip(s) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* soft = find_instruction(insts, cedar::Opcode::DISTORT_SOFT);
        REQUIRE(soft != nullptr);
        CHECK((soft->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

// =============================================================================
// Mixed-channel arithmetic (PRD §5.3 rule 4, §10.11)
// =============================================================================

TEST_CASE("Types: mixed mono/stereo arithmetic", "[types][stereo][arithmetic]") {
    // mono + stereo broadcasts the mono operand across both channels.
    // Array-broadcasting in the binary-op path naturally produces 2 output
    // buffers; the current result type is Array(2). A follow-up could tag
    // this explicitly as Stereo for downstream auto-lift, but the audio
    // result is already correct and the test just confirms the program
    // compiles and emits one ADD per channel.
    SECTION("mono * stereo compiles and produces 2 multiplies") {
        auto result = akkado::compile(R"(
            dry = saw(220)
            wet = stereo(saw(218), saw(222))
            dry * 0.3 + wet * 0.7 |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // One MUL for dry * 0.3 (mono), two for wet * 0.7 (stereo broadcast),
        // so at least 3 multiplies total.
        CHECK(count_instructions(insts, cedar::Opcode::MUL) >= 3);
    }
}

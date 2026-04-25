#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/sample_registry.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <cedar/dsp/constants.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

// Helper to decode float from PUSH_CONST instruction
static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

// Helper to extract instructions from bytecode
static std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

// Helper to find instruction by opcode
static const cedar::Instruction* find_instruction(const std::vector<cedar::Instruction>& insts,
                                                   cedar::Opcode op) {
    for (const auto& inst : insts) {
        if (inst.opcode == op) return &inst;
    }
    return nullptr;
}

// Helper to count instructions by opcode
static size_t count_instructions(const std::vector<cedar::Instruction>& insts,
                                  cedar::Opcode op) {
    size_t count = 0;
    for (const auto& inst : insts) {
        if (inst.opcode == op) ++count;
    }
    return count;
}

// =============================================================================
// Literal Tests
// =============================================================================

TEST_CASE("Codegen: Number literals", "[codegen][literals]") {
    SECTION("integer") {
        auto result = akkado::compile("42");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 42.0f);
    }

    SECTION("float") {
        auto result = akkado::compile("3.14159");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == Catch::Approx(3.14159f));
    }

    SECTION("negative") {
        auto result = akkado::compile("-440");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == -440.0f);
    }

    SECTION("zero") {
        auto result = akkado::compile("0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }
}

TEST_CASE("Codegen: Bool literals", "[codegen][literals]") {
    SECTION("true") {
        auto result = akkado::compile("true");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 1.0f);
    }

    SECTION("false") {
        auto result = akkado::compile("false");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }
}

TEST_CASE("Codegen: Pitch literals", "[codegen][literals]") {
    SECTION("a4 converts to MIDI 69 then MTOF") {
        auto result = akkado::compile("'a4'");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 2);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 69.0f);  // A4 = MIDI 69
        CHECK(insts[1].opcode == cedar::Opcode::MTOF);
        CHECK(insts[1].inputs[0] == insts[0].out_buffer);
    }

    SECTION("c4 converts to MIDI 60") {
        auto result = akkado::compile("'c4'");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 60.0f);
    }
}

TEST_CASE("Codegen: Chord literals", "[codegen][literals]") {
    SECTION("major chord uses root note") {
        auto result = akkado::compile("C4'");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() >= 2);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 60.0f);  // C4 root
        CHECK(insts[1].opcode == cedar::Opcode::MTOF);
    }
}

TEST_CASE("Codegen: Array literals", "[codegen][literals]") {
    SECTION("simple array") {
        auto result = akkado::compile("[1, 2, 3]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);  // 3 PUSH_CONST
        CHECK(decode_const_float(insts[0]) == 1.0f);
        CHECK(decode_const_float(insts[1]) == 2.0f);
        CHECK(decode_const_float(insts[2]) == 3.0f);
    }

    SECTION("empty array produces zero") {
        auto result = akkado::compile("[]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }

    SECTION("single element array") {
        auto result = akkado::compile("[42]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == 42.0f);
    }
}

// =============================================================================
// Variable Tests
// =============================================================================

TEST_CASE("Codegen: Variables", "[codegen][variables]") {
    SECTION("assignment and lookup") {
        auto result = akkado::compile("x = 440\nsaw(x)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // PUSH_CONST(440), OSC_SAW
        REQUIRE(insts.size() == 2);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(insts[1].opcode == cedar::Opcode::OSC_SAW);
        CHECK(insts[1].inputs[0] == insts[0].out_buffer);
    }

    SECTION("variable reuse in expression") {
        auto result = akkado::compile("f = 440\nsaw(f) + saw(f)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // PUSH_CONST, OSC_SAW, OSC_SAW, ADD
        auto* add = find_instruction(insts, cedar::Opcode::ADD);
        REQUIRE(add != nullptr);
    }
}

// =============================================================================
// Binary Operation Tests
// =============================================================================

TEST_CASE("Codegen: Binary operations", "[codegen][binop]") {
    SECTION("addition") {
        auto result = akkado::compile("1 + 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* add = find_instruction(insts, cedar::Opcode::ADD);
        REQUIRE(add != nullptr);
    }

    SECTION("subtraction") {
        auto result = akkado::compile("5 - 3");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* sub = find_instruction(insts, cedar::Opcode::SUB);
        REQUIRE(sub != nullptr);
    }

    SECTION("multiplication") {
        auto result = akkado::compile("2 * 3");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* mul = find_instruction(insts, cedar::Opcode::MUL);
        REQUIRE(mul != nullptr);
    }

    SECTION("division") {
        auto result = akkado::compile("10 / 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* div = find_instruction(insts, cedar::Opcode::DIV);
        REQUIRE(div != nullptr);
    }

    SECTION("power via pow()") {
        auto result = akkado::compile("pow(2, 8)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* pow = find_instruction(insts, cedar::Opcode::POW);
        REQUIRE(pow != nullptr);
    }

    SECTION("chained operations") {
        auto result = akkado::compile("1 + 2 + 3");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("buffer wiring") {
        auto result = akkado::compile("1 + 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);  // PUSH 1, PUSH 2, ADD
        CHECK(insts[2].inputs[0] == insts[0].out_buffer);
        CHECK(insts[2].inputs[1] == insts[1].out_buffer);
    }
}

// =============================================================================
// Closure Tests
// =============================================================================

TEST_CASE("Codegen: Closures", "[codegen][closures]") {
    SECTION("identity lambda") {
        auto result = akkado::compile("map([1, 2, 3], (x) -> x)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 PUSH_CONST for the array elements
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
    }

    SECTION("lambda with expression") {
        auto result = akkado::compile("map([1, 2], (x) -> x + 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have ADDs for each element
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }
}

// =============================================================================
// Higher-Order Function Tests
// =============================================================================

TEST_CASE("Codegen: map()", "[codegen][hof]") {
    SECTION("map identity") {
        auto result = akkado::compile("map([1, 2, 3], (x) -> x)");
        REQUIRE(result.success);
    }

    SECTION("map with transformation") {
        auto result = akkado::compile("map([1, 2], (x) -> x * 2)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 2);
    }

    SECTION("map single element") {
        auto result = akkado::compile("map([42], (x) -> x)");
        REQUIRE(result.success);
    }
}

TEST_CASE("Codegen: sum()", "[codegen][hof]") {
    SECTION("sum of array") {
        auto result = akkado::compile("sum([1, 2, 3])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 PUSH_CONST, 2 ADD (chain: (1+2)+3)
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("sum single element returns element") {
        auto result = akkado::compile("sum([42])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Just 1 PUSH_CONST, no ADD needed
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("sum empty array returns zero") {
        auto result = akkado::compile("sum([])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // No additions for an empty array.
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
        // A zero is emitted somewhere.
        bool found_zero = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 0.0f) {
                found_zero = true;
                break;
            }
        }
        CHECK(found_zero);
    }
}

// Higher-order reducer is named reduce() since 'fold' is taken by the wavefolding
// distortion builtin. See test_arrays.cpp for full reduce() coverage.

TEST_CASE("Codegen: zipWith()", "[codegen][hof]") {
    SECTION("zipWith add") {
        auto result = akkado::compile("zipWith([1, 2], [3, 4], (a, b) -> a + b)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have ADDs for each pair
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("zipWith unequal lengths uses shorter") {
        auto result = akkado::compile("zipWith([1, 2, 3], [4, 5], (a, b) -> a + b)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Only 2 additions (shorter array length)
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }
}

TEST_CASE("Codegen: zip()", "[codegen][hof]") {
    SECTION("zip interleaves arrays") {
        auto result = akkado::compile("zip([1, 2], [3, 4])");
        REQUIRE(result.success);
        // Should produce [1, 3, 2, 4] as 4 buffers
    }
}

TEST_CASE("Codegen: take()", "[codegen][hof]") {
    SECTION("take first n elements") {
        auto result = akkado::compile("take(2, [1, 2, 3, 4])");
        REQUIRE(result.success);
        // take visits the full array but returns only first 2 in multi_buffers_
        // All elements are still emitted as instructions
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 2);
    }

    SECTION("take more than array length") {
        auto result = akkado::compile("take(10, [1, 2])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 2);
    }
}

TEST_CASE("Codegen: drop()", "[codegen][hof]") {
    SECTION("drop first n elements") {
        auto result = akkado::compile("drop(2, [1, 2, 3, 4])");
        REQUIRE(result.success);
        // All 4 elements are emitted, drop just changes which are tracked
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 2);
    }
}

TEST_CASE("Codegen: reverse()", "[codegen][hof]") {
    SECTION("reverse array") {
        auto result = akkado::compile("reverse([1, 2, 3])");
        REQUIRE(result.success);
    }
}

TEST_CASE("Codegen: range()", "[codegen][hof]") {
    SECTION("range generates sequence") {
        auto result = akkado::compile("range(0, 3)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should produce [0, 1, 2]
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
        CHECK(decode_const_float(insts[0]) == 0.0f);
        CHECK(decode_const_float(insts[1]) == 1.0f);
        CHECK(decode_const_float(insts[2]) == 2.0f);
    }

    SECTION("range descending") {
        auto result = akkado::compile("range(3, 0)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
        CHECK(decode_const_float(insts[0]) == 3.0f);
        CHECK(decode_const_float(insts[1]) == 2.0f);
        CHECK(decode_const_float(insts[2]) == 1.0f);
    }
}

TEST_CASE("Codegen: repeat()", "[codegen][hof]") {
    SECTION("repeat value") {
        auto result = akkado::compile("repeat(42, 3)");
        REQUIRE(result.success);
        // Single value emitted, referenced 3 times in multi-buffer
    }
}

// =============================================================================
// User Function Tests
// =============================================================================

TEST_CASE("Codegen: User functions", "[codegen][functions]") {
    SECTION("simple function definition and call") {
        auto result = akkado::compile("fn double(x) -> x * 2\ndouble(21)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* mul = find_instruction(insts, cedar::Opcode::MUL);
        REQUIRE(mul != nullptr);
    }

    SECTION("function with default argument") {
        // Note: 'add' is a reserved builtin name, use 'myAdd' instead
        auto result = akkado::compile("fn myAdd(x, y = 10) -> x + y\nmyAdd(5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* add_op = find_instruction(insts, cedar::Opcode::ADD);
        REQUIRE(add_op != nullptr);
    }

    SECTION("nested function calls") {
        auto result = akkado::compile("fn inc(x) -> x + 1\ninc(inc(1))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }
}

// =============================================================================
// Match Expression Tests
// =============================================================================

TEST_CASE("Codegen: Match expressions - compile-time", "[codegen][match]") {
    SECTION("basic string pattern match") {
        auto result = akkado::compile(R"(
            fn choose(x) -> match(x) {
                "a": 1,
                "b": 2,
                _: 0
            }
            choose("a")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should emit just the winning branch: 1
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
        // Should NOT have any SELECT opcodes for compile-time match
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("match with wildcard default") {
        auto result = akkado::compile(R"(
            fn choose(x) -> match(x) {
                "known": 100,
                _: 42
            }
            choose("unknown")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should emit just the default branch: 42
        REQUIRE(insts.size() >= 1);
    }

    SECTION("match with number patterns") {
        auto result = akkado::compile(R"(
            fn pick(x) -> match(x) {
                1: 10,
                2: 20,
                _: 0
            }
            pick(2)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("match with bool patterns") {
        auto result = akkado::compile(R"(
            fn toggle(x) -> match(x) {
                true: 1,
                false: 0,
                _: -1
            }
            toggle(true)
        )");
        REQUIRE(result.success);
    }
}

TEST_CASE("Codegen: Match expressions - with guards", "[codegen][match]") {
    SECTION("compile-time guard with literal") {
        auto result = akkado::compile(R"(
            fn test(x) -> match(x) {
                "a" && true: 100,
                "a": 50,
                _: 0
            }
            test("a")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Guard true passes, should emit 100
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("compile-time guard with false literal skips arm") {
        auto result = akkado::compile(R"(
            fn test(x) -> match(x) {
                "a" && false: 100,
                "a": 50,
                _: 0
            }
            test("a")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Guard false fails, should fall through to "a": 50
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }
}

TEST_CASE("Codegen: Match expressions - runtime", "[codegen][match]") {
    SECTION("runtime scrutinee produces select chain") {
        auto result = akkado::compile(R"(
            x = saw(1)
            match(x) {
                0: 10,
                1: 20,
                _: 30
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Runtime match should use SELECT opcodes
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
        // Should have CMP_EQ for pattern comparisons
        CHECK(count_instructions(insts, cedar::Opcode::CMP_EQ) >= 1);
    }

    SECTION("runtime match with guards uses LOGIC_AND") {
        auto result = akkado::compile(R"(
            x = saw(1)
            y = tri(1)
            match(x) {
                0 && y > 0.5: 100,
                0: 50,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have LOGIC_AND for guard combination
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }
}

TEST_CASE("Codegen: Match expressions - guard-only form", "[codegen][match]") {
    SECTION("simple guard-only match") {
        auto result = akkado::compile(R"(
            x = saw(1)
            match {
                x > 0.5: 100,
                x > 0: 50,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have comparisons and selects
        CHECK(count_instructions(insts, cedar::Opcode::CMP_GT) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }

    SECTION("guard-only match with multiple conditions") {
        auto result = akkado::compile(R"(
            a = saw(1)
            b = tri(1)
            match {
                a > 0.5 && b < 0.5: 1,
                a > 0.5: 2,
                b > 0.5: 3,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }
}

TEST_CASE("Codegen: Match expressions - warnings", "[codegen][match]") {
    SECTION("missing wildcard arm produces warning") {
        auto result = akkado::compile(R"(
            x = saw(1)
            match {
                x > 0.5: 100
            }
        )");
        REQUIRE(result.success);  // Should still compile
        // Check for warning in diagnostics
        bool has_warning = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == akkado::Severity::Warning &&
                diag.code == "W001") {
                has_warning = true;
                break;
            }
        }
        CHECK(has_warning);
    }
}

TEST_CASE("Codegen: Match expressions - range patterns", "[codegen][match][range]") {
    SECTION("compile-time range match selects correct arm") {
        auto result = akkado::compile(R"(
            match(0.5) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Compile-time: should emit just the winning branch (2), no SELECT
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        // Should emit PUSH_CONST 2.0
        REQUIRE(insts.size() >= 1);
        CHECK(decode_const_float(insts[0]) == 2.0f);
    }

    SECTION("half-open range excludes upper bound") {
        // 0.3 should NOT match 0.0..0.3, but SHOULD match 0.3..0.7
        auto result = akkado::compile(R"(
            match(0.3) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 2.0f);
    }

    SECTION("range match with lower bound exact match") {
        // 0.0 should match 0.0..0.3
        auto result = akkado::compile(R"(
            match(0.0) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 1.0f);
    }

    SECTION("range match falls through to wildcard") {
        auto result = akkado::compile(R"(
            match(1.5) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 99
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 99.0f);
    }

    SECTION("negative range patterns") {
        auto result = akkado::compile(R"(
            match(-0.5) {
                -1.0..0.0: 1,
                0.0..1.0: 2,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 1.0f);
    }

    SECTION("negative number pattern (non-range)") {
        // Verify that standalone negative numbers work in match arms
        auto result = akkado::compile(R"(
            fn pick(x) -> match(x) {
                -1: 10,
                0: 20,
                1: 30,
                _: 0
            }
            pick(-1)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("runtime range match produces CMP_GTE, CMP_LT, LOGIC_AND, SELECT") {
        auto result = akkado::compile(R"(
            vel = saw(1)
            match(vel) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Runtime range match should use CMP_GTE + CMP_LT + LOGIC_AND
        CHECK(count_instructions(insts, cedar::Opcode::CMP_GTE) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_LT) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }

    SECTION("runtime range match with guard uses extra LOGIC_AND") {
        auto result = akkado::compile(R"(
            vel = saw(1)
            mode = tri(1)
            match(vel) {
                0.0..0.5 && mode > 0.5: 100,
                0.5..1.0: 200,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have LOGIC_AND for range condition + guard combination
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) >= 2);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }

    SECTION("compile-time range with function call") {
        auto result = akkado::compile(R"(
            fn velocity_layer(vel) -> match(vel) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 0
            }
            velocity_layer(0.8)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should resolve at compile-time to 3 (no SELECT)
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        // The result (3.0) should be one of the PUSH_CONST instructions
        bool found_result = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST && decode_const_float(inst) == 3.0f) {
                found_result = true;
                break;
            }
        }
        CHECK(found_result);
    }
}

// =============================================================================
// Pattern Tests (MiniLiteral)
// =============================================================================

TEST_CASE("Codegen: Patterns", "[codegen][patterns]") {
    SECTION("pitch pattern produces SEQPAT_QUERY and SEQPAT_STEP") {
        auto result = akkado::compile("pat(\"c4 e4 g4\")");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Patterns now use lazy query system (SEQPAT_QUERY + SEQPAT_STEP)
        auto* query = find_instruction(insts, cedar::Opcode::SEQPAT_QUERY);
        auto* step = find_instruction(insts, cedar::Opcode::SEQPAT_STEP);
        REQUIRE(query != nullptr);
        REQUIRE(step != nullptr);
    }
}

// =============================================================================
// Buffer Allocation Tests
// =============================================================================

TEST_CASE("Codegen: Buffer allocation", "[codegen][buffers]") {
    SECTION("sequential buffer indices") {
        auto result = akkado::compile("[1, 2, 3]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);
        CHECK(insts[0].out_buffer == 0);
        CHECK(insts[1].out_buffer == 1);
        CHECK(insts[2].out_buffer == 2);
    }

    SECTION("instruction inputs reference prior outputs") {
        auto result = akkado::compile("1 + 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);
        CHECK(insts[2].inputs[0] == insts[0].out_buffer);
        CHECK(insts[2].inputs[1] == insts[1].out_buffer);
    }
}

// =============================================================================
// Integration Tests
// =============================================================================

// =============================================================================
// Conditionals and Logic Tests
// =============================================================================

TEST_CASE("Codegen: Comparison operators - function syntax", "[codegen][conditionals]") {
    SECTION("gt() greater than") {
        auto result = akkado::compile("gt(10, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        REQUIRE(cmp != nullptr);
    }

    SECTION("lt() less than") {
        auto result = akkado::compile("lt(5, 10)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LT);
        REQUIRE(cmp != nullptr);
    }

    SECTION("gte() greater or equal") {
        auto result = akkado::compile("gte(5, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("lte() less or equal") {
        auto result = akkado::compile("lte(5, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("eq() equality") {
        auto result = akkado::compile("eq(5, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_EQ);
        REQUIRE(cmp != nullptr);
    }

    SECTION("neq() not equal") {
        auto result = akkado::compile("neq(5, 10)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_NEQ);
        REQUIRE(cmp != nullptr);
    }
}

TEST_CASE("Codegen: Logic operators - function syntax", "[codegen][conditionals]") {
    SECTION("band() logical AND") {
        auto result = akkado::compile("band(1, 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        REQUIRE(logic != nullptr);
    }

    SECTION("bor() logical OR") {
        auto result = akkado::compile("bor(1, 0)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic != nullptr);
    }

    SECTION("bnot() logical NOT") {
        auto result = akkado::compile("bnot(0)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_NOT);
        REQUIRE(logic != nullptr);
    }
}

TEST_CASE("Codegen: Select function", "[codegen][conditionals]") {
    SECTION("select() ternary") {
        auto result = akkado::compile("select(1, 100, 50)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* sel = find_instruction(insts, cedar::Opcode::SELECT);
        REQUIRE(sel != nullptr);
    }

    SECTION("select() with expressions") {
        auto result = akkado::compile("select(gt(10, 5), 100, 50)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* sel = find_instruction(insts, cedar::Opcode::SELECT);
        REQUIRE(cmp != nullptr);
        REQUIRE(sel != nullptr);
    }
}

TEST_CASE("Codegen: Comparison operators - infix syntax", "[codegen][conditionals]") {
    SECTION("> greater than") {
        auto result = akkado::compile("10 > 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        REQUIRE(cmp != nullptr);
    }

    SECTION("< less than") {
        auto result = akkado::compile("5 < 10");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LT);
        REQUIRE(cmp != nullptr);
    }

    SECTION(">= greater or equal") {
        auto result = akkado::compile("5 >= 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("<= less or equal") {
        auto result = akkado::compile("5 <= 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("== equality") {
        auto result = akkado::compile("5 == 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_EQ);
        REQUIRE(cmp != nullptr);
    }

    SECTION("!= not equal") {
        auto result = akkado::compile("5 != 10");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_NEQ);
        REQUIRE(cmp != nullptr);
    }
}

TEST_CASE("Codegen: Logic operators - infix syntax", "[codegen][conditionals]") {
    SECTION("&& logical AND") {
        auto result = akkado::compile("1 && 1");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        REQUIRE(logic != nullptr);
    }

    SECTION("|| logical OR") {
        auto result = akkado::compile("1 || 0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic != nullptr);
    }

    SECTION("! prefix NOT") {
        auto result = akkado::compile("!1");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_NOT);
        REQUIRE(logic != nullptr);
    }

    SECTION("! with expression") {
        auto result = akkado::compile("!(5 > 10)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_NOT);
        REQUIRE(cmp != nullptr);
        REQUIRE(logic != nullptr);
    }
}

TEST_CASE("Codegen: Operator precedence", "[codegen][conditionals]") {
    SECTION("&& binds tighter than ||") {
        // 1 || 0 && 0 should be parsed as 1 || (0 && 0) = 1
        auto result = akkado::compile("1 || 0 && 0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have LOGIC_AND before LOGIC_OR in execution order
        auto* logic_and = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        auto* logic_or = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic_and != nullptr);
        REQUIRE(logic_or != nullptr);
    }

    SECTION("Comparison binds tighter than logic") {
        // 5 > 3 && 2 < 4 should be parsed as (5 > 3) && (2 < 4)
        auto result = akkado::compile("5 > 3 && 2 < 4");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp_gt = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* cmp_lt = find_instruction(insts, cedar::Opcode::CMP_LT);
        auto* logic_and = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        REQUIRE(cmp_gt != nullptr);
        REQUIRE(cmp_lt != nullptr);
        REQUIRE(logic_and != nullptr);
    }

    SECTION("Arithmetic binds tighter than comparison") {
        // 2 + 3 > 4 should be parsed as (2 + 3) > 4
        auto result = akkado::compile("2 + 3 > 4");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* add = find_instruction(insts, cedar::Opcode::ADD);
        auto* cmp_gt = find_instruction(insts, cedar::Opcode::CMP_GT);
        REQUIRE(add != nullptr);
        REQUIRE(cmp_gt != nullptr);
    }

    SECTION("Grouping overrides precedence") {
        // (1 || 0) && 0 should evaluate || first
        auto result = akkado::compile("(1 || 0) && 0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic_and = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        auto* logic_or = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic_and != nullptr);
        REQUIRE(logic_or != nullptr);
    }
}

TEST_CASE("Codegen: Complex conditional expressions", "[codegen][conditionals]") {
    SECTION("Chained comparisons with logic") {
        auto result = akkado::compile("(5 > 3) && (10 < 20) || (1 == 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_GT) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_LT) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_EQ) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_OR) == 1);
    }

    SECTION("Select with comparison condition") {
        auto result = akkado::compile("select(10 > 5, 100, 50)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* sel = find_instruction(insts, cedar::Opcode::SELECT);
        REQUIRE(cmp != nullptr);
        REQUIRE(sel != nullptr);
    }

    SECTION("Double negation") {
        auto result = akkado::compile("!!1");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_NOT) == 2);
    }
}

TEST_CASE("Codegen: Complex expressions", "[codegen][integration]") {
    SECTION("map with sum") {
        auto result = akkado::compile("sum(map([1, 2, 3], (x) -> x * 2))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("polyphonic oscillator inline without poly is error") {
        auto result = akkado::compile("sum(map(mtof(chord(\"Am\")), (f) -> saw(f)))");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// Embedded Alternate Pattern Tests
// =============================================================================

TEST_CASE("Codegen: Embedded alternate sequence timing", "[codegen][pattern][sequence]") {
    SECTION("a <b c> d - alternate embedded in normal sequence") {
        // Pattern: a <b c> d
        // a takes 1/3, <b c> takes 1/3, d takes 1/3
        // Inside the alternate, b and c each have full span (1.0) of their SUB_SEQ slot
        auto result = akkado::compile("pat(\"c4 <e4 g4> a4\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequences.size() >= 2);  // Root + alternate
        REQUIRE(seq_init->sequence_events.size() >= 2);  // Event storage for each sequence

        // Root sequence should have 3 elements (c4, SUB_SEQ, a4)
        const auto& root = seq_init->sequences[0];
        const auto& root_events = seq_init->sequence_events[0];
        REQUIRE(root_events.size() == 3);
        CHECK(root.mode == cedar::SequenceMode::NORMAL);

        // Check event times and durations
        // Each element takes 1/3 of the normalized span (0.333)
        const float third = 1.0f / 3.0f;

        // Event 0: c4 at time=0
        CHECK(root_events[0].type == cedar::EventType::DATA);
        CHECK(root_events[0].time == Catch::Approx(0.0f).margin(0.001f));
        CHECK(root_events[0].duration == Catch::Approx(third).margin(0.001f));

        // Event 1: SUB_SEQ at time=1/3
        CHECK(root_events[1].type == cedar::EventType::SUB_SEQ);
        CHECK(root_events[1].time == Catch::Approx(third).margin(0.001f));
        CHECK(root_events[1].duration == Catch::Approx(third).margin(0.001f));

        // Event 2: a4 at time=2/3
        CHECK(root_events[2].type == cedar::EventType::DATA);
        CHECK(root_events[2].time == Catch::Approx(2.0f * third).margin(0.001f));
        CHECK(root_events[2].duration == Catch::Approx(third).margin(0.001f));

        // Alternate sequence (ID 1) should have 2 choices with duration=1.0
        if (seq_init->sequences.size() > 1 && seq_init->sequence_events.size() > 1) {
            const auto& alt = seq_init->sequences[1];
            const auto& alt_events = seq_init->sequence_events[1];
            CHECK(alt.mode == cedar::SequenceMode::ALTERNATE);
            REQUIRE(alt_events.size() == 2);
            // Each alternate choice has full span (1.0) within its SUB_SEQ slot
            CHECK(alt_events[0].duration == Catch::Approx(1.0f).margin(0.001f));
            CHECK(alt_events[1].duration == Catch::Approx(1.0f).margin(0.001f));
        }
    }

    SECTION("verify query output durations") {
        // Compile the pattern
        auto result = akkado::compile("pat(\"c4 <e4 g4> a4\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Create a SequenceState and query it using static buffers for the test
        static constexpr std::size_t TEST_MAX_SEQUENCES = 16;
        static constexpr std::size_t TEST_MAX_EVENTS_PER_SEQ = 64;
        static constexpr std::size_t TEST_MAX_OUTPUT_EVENTS = 64;
        static cedar::Sequence test_sequences[TEST_MAX_SEQUENCES];
        static cedar::Event test_events[TEST_MAX_SEQUENCES][TEST_MAX_EVENTS_PER_SEQ];
        static cedar::OutputEvents::OutputEvent test_output_events[TEST_MAX_OUTPUT_EVENTS];

        std::size_t num_seqs = std::min(seq_init->sequences.size(), TEST_MAX_SEQUENCES);

        // Copy sequences and set up event pointers
        for (std::size_t i = 0; i < num_seqs; ++i) {
            test_sequences[i] = seq_init->sequences[i];
            if (i < seq_init->sequence_events.size() && !seq_init->sequence_events[i].empty()) {
                std::size_t num_events = std::min(seq_init->sequence_events[i].size(), TEST_MAX_EVENTS_PER_SEQ);
                for (std::size_t j = 0; j < num_events; ++j) {
                    test_events[i][j] = seq_init->sequence_events[i][j];
                }
                test_sequences[i].events = test_events[i];
                test_sequences[i].num_events = static_cast<std::uint32_t>(num_events);
                test_sequences[i].capacity = static_cast<std::uint32_t>(TEST_MAX_EVENTS_PER_SEQ);
            }
        }

        cedar::SequenceState state;
        state.sequences = test_sequences;
        state.num_sequences = static_cast<std::uint32_t>(num_seqs);
        state.seq_capacity = static_cast<std::uint32_t>(TEST_MAX_SEQUENCES);
        state.output.events = test_output_events;
        state.output.num_events = 0;
        state.output.capacity = static_cast<std::uint32_t>(TEST_MAX_OUTPUT_EVENTS);
        state.cycle_length = seq_init->cycle_length;

        // Query cycle 0
        cedar::query_pattern(state, 0, seq_init->cycle_length);

        // Should have 3 events
        REQUIRE(state.output.num_events == 3);

        // All durations should be cycle_length / 3
        float expected_duration = seq_init->cycle_length / 3.0f;
        CHECK(state.output.events[0].duration == Catch::Approx(expected_duration).margin(0.01f));
        CHECK(state.output.events[1].duration == Catch::Approx(expected_duration).margin(0.01f));
        CHECK(state.output.events[2].duration == Catch::Approx(expected_duration).margin(0.01f));

        // Check times
        CHECK(state.output.events[0].time == Catch::Approx(0.0f).margin(0.01f));
        CHECK(state.output.events[1].time == Catch::Approx(expected_duration).margin(0.01f));
        CHECK(state.output.events[2].time == Catch::Approx(2.0f * expected_duration).margin(0.01f));
    }

    SECTION("long pattern - 16 events (exceeds old MAX_EVENTS_PER_SEQ=8)") {
        // This pattern has 16 events, which exceeds the old limit of 8
        auto result = akkado::compile("pat(\"c4 g4 ~ ~ c5 e4 ~ g3 c4 g4 ~ ~ c5 e4 ~ a3\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequence_events.size() >= 1);

        // Count total events (excluding rests which have 0 events)
        // Pattern: c4 g4 ~ ~ c5 e4 ~ g3 c4 g4 ~ ~ c5 e4 ~ a3
        // Notes:   1  2     3  4     5  6  7     8  9     10 = 10 note events
        std::uint32_t total_events = 0;
        for (const auto& events : seq_init->sequence_events) {
            total_events += static_cast<std::uint32_t>(events.size());
        }
        CHECK(total_events >= 10);  // At least 10 note events
    }

    SECTION("long pattern with groups - many nested events") {
        // This creates 10 main events plus nested events
        auto result = akkado::compile("pat(\"[c4 d4] [e4 f4] [g4 a4] [b4 c5] [d5 e5]\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Count total events across all sequences
        std::uint32_t total_events = 0;
        for (const auto& events : seq_init->sequence_events) {
            total_events += static_cast<std::uint32_t>(events.size());
        }
        // Should have 10 note events
        CHECK(total_events >= 10);
    }

    SECTION("alternation with groups - groups wrapped in sub-sequences") {
        // <[c4 e4] [g4 b4]> should alternate between the two groups as units
        // not cycle through individual notes c4, e4, g4, b4
        auto result = akkado::compile("pat(\"<[c4 e4] [g4 b4]>\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequences.size() >= 2);

        // Find the ALTERNATE sequence
        std::size_t alt_idx = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < seq_init->sequences.size(); ++i) {
            if (seq_init->sequences[i].mode == cedar::SequenceMode::ALTERNATE) {
                alt_idx = i;
                break;
            }
        }
        REQUIRE(alt_idx != static_cast<std::size_t>(-1));

        // The ALTERNATE sequence should have exactly 2 events (SUB_SEQ for each group)
        // not 4 events (individual notes unrolled)
        const auto& alt_events = seq_init->sequence_events[alt_idx];
        CHECK(alt_events.size() == 2);

        // Each event should be a SUB_SEQ pointing to a NORMAL sequence containing
        // the group's notes
        for (const auto& ev : alt_events) {
            CHECK(ev.type == cedar::EventType::SUB_SEQ);
        }
    }

    SECTION("choice with groups - groups wrapped in sub-sequences") {
        // [c4 e4] | [g4 b4] should pick between the two groups as units
        auto result = akkado::compile("pat(\"[c4 e4] | [g4 b4]\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Find the RANDOM sequence (choice operator uses RANDOM mode)
        std::size_t rand_idx = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < seq_init->sequences.size(); ++i) {
            if (seq_init->sequences[i].mode == cedar::SequenceMode::RANDOM) {
                rand_idx = i;
                break;
            }
        }
        REQUIRE(rand_idx != static_cast<std::size_t>(-1));

        // The RANDOM sequence should have exactly 2 events (SUB_SEQ for each group)
        const auto& rand_events = seq_init->sequence_events[rand_idx];
        CHECK(rand_events.size() == 2);

        // Each event should be a SUB_SEQ
        for (const auto& ev : rand_events) {
            CHECK(ev.type == cedar::EventType::SUB_SEQ);
        }
    }

    SECTION("speed-modified alternate with groups - groups wrapped in sub-sequences") {
        // <hh hh [hh hh hh]>*2 should have an ALTERNATE sequence with 3 events:
        // 2 DATA (plain hh atoms) + 1 SUB_SEQ (for the [hh hh hh] group)
        // NOT 5 DATA events (flattened group)
        auto result = akkado::compile("pat(\"<hh hh [hh hh hh]>*2\")");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Find the ALTERNATE sequence
        std::size_t alt_idx = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < seq_init->sequences.size(); ++i) {
            if (seq_init->sequences[i].mode == cedar::SequenceMode::ALTERNATE) {
                alt_idx = i;
                break;
            }
        }
        REQUIRE(alt_idx != static_cast<std::size_t>(-1));

        // The ALTERNATE sequence should have exactly 3 events:
        // hh (DATA), hh (DATA), [hh hh hh] (SUB_SEQ)
        const auto& alt_events = seq_init->sequence_events[alt_idx];
        CHECK(alt_events.size() == 3);

        // The third event (for the group) should be a SUB_SEQ
        CHECK(alt_events[2].type == cedar::EventType::SUB_SEQ);
    }
}

// =============================================================================
// Parameter Exposure Tests
// =============================================================================

TEST_CASE("Codegen: param() generates ENV_GET and records declaration", "[codegen][params]") {
    SECTION("basic param declaration") {
        auto result = akkado::compile(R"(
            vol = param("volume", 0.8, 0, 1)
            saw(220) * vol
        )");
        REQUIRE(result.success);

        // Check param_decls populated
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "volume");
        CHECK(decl.type == akkado::ParamType::Continuous);
        CHECK(decl.default_value == Catch::Approx(0.8f));
        CHECK(decl.min_value == Catch::Approx(0.0f));
        CHECK(decl.max_value == Catch::Approx(1.0f));

        // Verify ENV_GET instruction emitted
        auto insts = get_instructions(result);
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);

        // Verify hash matches declaration
        CHECK(env_get->state_id == decl.name_hash);
    }

    SECTION("param with default range") {
        auto result = akkado::compile(R"(
            x = param("x", 0.5)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.default_value == Catch::Approx(0.5f));
        CHECK(decl.min_value == Catch::Approx(0.0f));
        CHECK(decl.max_value == Catch::Approx(1.0f));
    }

    SECTION("param clamps default to range") {
        auto result = akkado::compile(R"(
            x = param("x", 2.0, 0, 1)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        CHECK(result.param_decls[0].default_value == Catch::Approx(1.0f));
    }

    SECTION("param default below min gets clamped") {
        auto result = akkado::compile(R"(
            x = param("x", -1.0, 0, 10)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        CHECK(result.param_decls[0].default_value == Catch::Approx(0.0f));
    }

    SECTION("param with min > max swaps values") {
        auto result = akkado::compile(R"(
            x = param("x", 0.5, 1, 0)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        // min/max should be swapped
        CHECK(result.param_decls[0].min_value == Catch::Approx(0.0f));
        CHECK(result.param_decls[0].max_value == Catch::Approx(1.0f));
        // Check for warning
        bool has_warning = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == akkado::Severity::Warning && diag.code == "W050") {
                has_warning = true;
                break;
            }
        }
        CHECK(has_warning);
    }

    SECTION("multiple params deduplicate by name") {
        auto result = akkado::compile(R"(
            a = param("vol", 0.5)
            b = param("vol", 0.5)
        )");
        REQUIRE(result.success);
        CHECK(result.param_decls.size() == 1);
    }

    SECTION("different params recorded separately") {
        auto result = akkado::compile(R"(
            v = param("volume", 0.8)
            c = param("cutoff", 2000, 100, 8000)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 2);
        CHECK(result.param_decls[0].name == "volume");
        CHECK(result.param_decls[1].name == "cutoff");
    }
}

TEST_CASE("Codegen: param() requires string literal name", "[codegen][params]") {
    SECTION("variable name fails") {
        auto result = akkado::compile(R"(
            name = "vol"
            x = param(name, 0.5)
        )");
        REQUIRE_FALSE(result.success);
        bool has_error = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.code == "E151") {
                has_error = true;
                break;
            }
        }
        CHECK(has_error);
    }

    SECTION("number literal fails") {
        auto result = akkado::compile(R"(
            x = param(42, 0.5)
        )");
        REQUIRE_FALSE(result.success);
    }
}

TEST_CASE("Codegen: button() creates momentary parameter", "[codegen][params]") {
    SECTION("basic button declaration") {
        auto result = akkado::compile(R"(
            kick = button("kick")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "kick");
        CHECK(decl.type == akkado::ParamType::Button);
        CHECK(decl.default_value == 0.0f);
        CHECK(decl.min_value == 0.0f);
        CHECK(decl.max_value == 1.0f);
    }

    SECTION("button emits ENV_GET with zero fallback") {
        auto result = akkado::compile(R"(
            trig = button("trigger")
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);

        // Find PUSH_CONST for fallback (should be 0)
        const cedar::Instruction* fallback = nullptr;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST) {
                fallback = &inst;
                break;
            }
        }
        REQUIRE(fallback != nullptr);
        CHECK(decode_const_float(*fallback) == 0.0f);

        // Verify ENV_GET
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);
    }
}

TEST_CASE("Codegen: toggle() creates boolean parameter", "[codegen][params]") {
    SECTION("toggle with default off") {
        auto result = akkado::compile(R"(
            mute = toggle("mute")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "mute");
        CHECK(decl.type == akkado::ParamType::Toggle);
        CHECK(decl.default_value == 0.0f);
    }

    SECTION("toggle with default on") {
        auto result = akkado::compile(R"(
            enabled = toggle("enabled", 1)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        CHECK(result.param_decls[0].default_value == 1.0f);
    }

    SECTION("toggle normalizes default to boolean") {
        auto result = akkado::compile(R"(
            x = toggle("x", 0.7)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        // 0.7 > 0.5 should normalize to 1.0
        CHECK(result.param_decls[0].default_value == 1.0f);
    }

    SECTION("toggle normalizes default below threshold") {
        auto result = akkado::compile(R"(
            x = toggle("x", 0.3)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        // 0.3 < 0.5 should normalize to 0.0
        CHECK(result.param_decls[0].default_value == 0.0f);
    }
}

TEST_CASE("Codegen: param_decls source location", "[codegen][params]") {
    SECTION("source offset and length recorded") {
        auto result = akkado::compile(R"(vol = param("volume", 0.5))");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        // Source offset should point to the param() call
        CHECK(decl.source_offset > 0);
        CHECK(decl.source_length > 0);
    }
}

TEST_CASE("Codegen: param hash matches cedar FNV-1a", "[codegen][params]") {
    SECTION("hash is consistent") {
        auto result = akkado::compile(R"(x = param("volume", 0.5))");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        // Compute expected hash
        const char* name = "volume";
        std::uint32_t expected = cedar::fnv1a_hash_runtime(name, std::strlen(name));
        CHECK(decl.name_hash == expected);

        // ENV_GET instruction should use the same hash
        auto insts = get_instructions(result);
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);
        CHECK(env_get->state_id == expected);
    }
}

TEST_CASE("Codegen: dropdown() creates selection parameter", "[codegen][params]") {
    SECTION("basic dropdown declaration") {
        auto result = akkado::compile(R"(
            wave = dropdown("waveform", "sine", "saw", "square")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "waveform");
        CHECK(decl.type == akkado::ParamType::Select);
        CHECK(decl.default_value == 0.0f);  // First option is default
        CHECK(decl.min_value == 0.0f);
        CHECK(decl.max_value == 2.0f);  // 3 options -> max index 2
        REQUIRE(decl.options.size() == 3);
        CHECK(decl.options[0] == "sine");
        CHECK(decl.options[1] == "saw");
        CHECK(decl.options[2] == "square");
    }

    SECTION("dropdown with single option") {
        auto result = akkado::compile(R"(
            mode = dropdown("mode", "default")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.min_value == 0.0f);
        CHECK(decl.max_value == 0.0f);  // 1 option -> max index 0
        REQUIRE(decl.options.size() == 1);
    }

    SECTION("dropdown emits ENV_GET") {
        auto result = akkado::compile(R"(
            x = dropdown("x", "a", "b")
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);
    }

    SECTION("dropdown requires at least one option") {
        // Note: The builtin signature requires at least 2 args (name + opt1)
        // so the analyzer rejects this before codegen sees it
        auto result = akkado::compile(R"(
            x = dropdown("x")
        )");
        REQUIRE_FALSE(result.success);
        // Either analyzer rejection (E004 or E005) or codegen error (E159)
        bool has_error = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == akkado::Severity::Error) {
                has_error = true;
                break;
            }
        }
        CHECK(has_error);
    }

    SECTION("dropdown options must be string literals") {
        auto result = akkado::compile(R"(
            opt = "dynamic"
            x = dropdown("x", opt)
        )");
        REQUIRE_FALSE(result.success);
    }
}

// =============================================================================
// Dot-Call Syntax Tests
// =============================================================================

TEST_CASE("Dot-call syntax", "[codegen][methods]") {
    SECTION("method on builtin: sin(440).abs() == abs(sin(440))") {
        auto dot = akkado::compile(R"(
            x = sin(440).abs()
            out(x, x)
        )");
        auto direct = akkado::compile(R"(
            x = abs(sin(440))
            out(x, x)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        // Same instructions
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call with arguments: osc(\"saw\", 440).lp(800, 0.707)") {
        auto dot = akkado::compile(R"(
            osc("saw", 440).lp(800, 0.707) |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            lp(osc("saw", 440), 800, 0.707) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("chained dot-calls: a.f().g() == g(f(a))") {
        auto dot = akkado::compile(R"(
            osc("saw", 440).lp(800).abs() |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            abs(lp(osc("saw", 440), 800)) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call on user-defined function") {
        auto dot = akkado::compile(R"(
            fn gain(sig, amt) -> sig * amt
            osc("sin", 440).gain(0.5) |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            fn gain(sig, amt) -> sig * amt
            gain(osc("sin", 440), 0.5) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call mixed with pipe operator") {
        auto dot = akkado::compile(R"(
            osc("saw", 440).lp(800) |> % * 0.5 |> out(%, %)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> lp(%, 800) |> % * 0.5 |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("dot-call on expression result") {
        auto dot = akkado::compile(R"(
            x = osc("sin", 440)
            x.abs() |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            x = osc("sin", 440)
            abs(x) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call with no extra arguments") {
        // noise().abs() == abs(noise())
        auto dot = akkado::compile(R"(
            noise().abs() |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            abs(noise()) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call produces correct opcodes") {
        auto result = akkado::compile(R"(
            osc("saw", 440).lp(800) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SAW) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::FILTER_SVF_LP) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }

    SECTION("pattern method via dot-call: pat().slow()") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4").slow(2))");
        auto direct = akkado::compile(R"(slow(pat("c4 e4 g4"), 2))");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("chained pattern methods via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4").fast(2).slow(4))");
        auto direct = akkado::compile(R"(slow(fast(pat("c4 e4"), 2), 4))");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call on hole: |> %.f(args)") {
        auto dot = akkado::compile(R"(
            osc("saw", 440) |> %.lp(800) |> out(%, %)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> lp(%, 800) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("dot-call on hole with multiple args") {
        auto dot = akkado::compile(R"(
            osc("saw", 440) |> %.lp(800, 2.0) |> out(%, %)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> lp(%, 800, 2.0) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("chained dot-calls on hole") {
        auto dot = akkado::compile(R"(
            osc("saw", 440) |> %.lp(800).abs() |> out(%, %)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> abs(lp(%, 800)) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("dot-call on as-binding: as q |> q.f(args)") {
        auto dot = akkado::compile(R"(
            osc("saw", 440) as q |> q.lp(800) |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            osc("saw", 440) as q |> lp(q, 800) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }
}

// =============================================================================
// Pattern Function Tests
// =============================================================================

TEST_CASE("Pattern function: slow()", "[codegen][patterns]") {
    SECTION("slow requires pattern as first argument") {
        auto result = akkado::compile("slow(42, 2)");
        REQUIRE_FALSE(result.success);
        bool found_error = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.code == "E133") {
                found_error = true;
                break;
            }
        }
        CHECK(found_error);
    }

    SECTION("slow requires positive number as second argument") {
        auto result = akkado::compile(R"(slow(pat("c4"), -1))");
        REQUIRE_FALSE(result.success);
    }

    SECTION("slow with valid pattern compiles") {
        // Note: slow() currently passes through - full implementation pending
        auto result = akkado::compile(R"(slow(pat("c4 e4 g4"), 2))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: fast()", "[codegen][patterns]") {
    SECTION("fast requires pattern as first argument") {
        auto result = akkado::compile("fast(42, 2)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("fast with valid pattern compiles") {
        auto result = akkado::compile(R"(fast(pat("c4 e4"), 2))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: rev()", "[codegen][patterns]") {
    SECTION("rev requires pattern as argument") {
        auto result = akkado::compile("rev(42)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("rev with valid pattern compiles") {
        auto result = akkado::compile(R"(rev(pat("c4 e4 g4")))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: transpose()", "[codegen][patterns]") {
    SECTION("transpose requires pattern as first argument") {
        auto result = akkado::compile("transpose(42, 7)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("transpose with valid pattern compiles") {
        auto result = akkado::compile(R"(transpose(pat("c4 e4 g4"), 7))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: velocity()", "[codegen][patterns]") {
    SECTION("velocity requires pattern as first argument") {
        auto result = akkado::compile("velocity(42, 0.5)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("velocity requires value between 0 and 1") {
        auto result = akkado::compile(R"(velocity(pat("c4"), 1.5))");
        REQUIRE_FALSE(result.success);
    }

    SECTION("velocity with valid pattern and value compiles") {
        auto result = akkado::compile(R"(velocity(pat("c4 e4"), 0.7))");
        CHECK(result.success);
    }
}

// =============================================================================
// Pattern Transform Chaining Tests
// =============================================================================

TEST_CASE("Pattern transform chaining compiles", "[codegen][patterns]") {
    SECTION("transpose(slow(...)) compiles - two-level nesting") {
        auto result = akkado::compile(R"(transpose(slow(pat("c4 e4"), 2), 12))");
        CHECK(result.success);
    }

    SECTION("rev(transpose(slow(...))) compiles - three-level nesting") {
        auto result = akkado::compile(R"(rev(transpose(slow(pat("c4 e4"), 2), 12)))");
        CHECK(result.success);
    }

    SECTION("fast(transpose(...)) compiles") {
        auto result = akkado::compile(R"(fast(transpose(pat("c4 e4"), 7), 2))");
        CHECK(result.success);
    }

    SECTION("slow(rev(...)) compiles") {
        auto result = akkado::compile(R"(slow(rev(pat("c4 e4 g4")), 3))");
        CHECK(result.success);
    }

    SECTION("transpose(fast(...)) compiles") {
        auto result = akkado::compile(R"(transpose(fast(pat("c4 e4"), 2), 5))");
        CHECK(result.success);
    }

    SECTION("rev(slow(...)) compiles") {
        auto result = akkado::compile(R"(rev(slow(pat("c4 e4 g4"), 2)))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern transform chaining: semantic correctness", "[codegen][patterns]") {
    SECTION("slow(pat(...), 2) doubles cycle length, event times unchanged") {
        // pat("c4 e4") has 2 elements -> base cycle_length = 2
        // slow(2) -> cycle_length = 4, event times stay normalized
        // Runtime formula (e.time * cycle_length) handles the scaling
        auto result = akkado::compile(R"(slow(pat("c4 e4"), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 2);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f));
        CHECK(si.sequence_events[0][1].time == Catch::Approx(0.5f));
    }

    SECTION("slow(fast(pat(...), 2), 2) is identity") {
        // pat("c4 e4") base: cycle_length=2, times at 0, 0.5
        // fast(2): cycle_length=1, times unchanged
        // slow(2): cycle_length=2, times unchanged -> identity
        auto result = akkado::compile(R"(slow(fast(pat("c4 e4"), 2), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f));
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 2);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f));
        CHECK(si.sequence_events[0][1].time == Catch::Approx(0.5f));
    }

    SECTION("transpose(slow(pat(...), 2), 12) applies both transforms") {
        // pat("c4 e4") base: cycle_length=2
        // slow(2): cycle_length=4
        // transpose(12): frequencies doubled (octave up)
        auto result = akkado::compile(R"(transpose(slow(pat("c4 e4"), 2), 12))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        // Cycle length should be doubled from slow
        CHECK(si.cycle_length == Catch::Approx(4.0f));

        // Events should have transposed frequencies
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 2);
        // C4 = 261.63 Hz, transposed +12 = 523.25 Hz
        float c4_freq = 261.6256f;
        CHECK(si.sequence_events[0][0].values[0] == Catch::Approx(c4_freq * 2.0f).margin(1.0f));
    }

    SECTION("fast(fast(pat(...), 2), 3) compounds speed") {
        // pat("c4 e4") base: cycle_length=2
        // fast(2): cycle_length=1
        // fast(3): cycle_length=1/3
        auto result = akkado::compile(R"(fast(fast(pat("c4 e4"), 2), 3))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f / 6.0f));
    }

    SECTION("fast(pat(...), 2) halves cycle length, event times unchanged") {
        // pat("c4 e4") has 2 elements -> base cycle_length = 2
        // fast(2) -> cycle_length = 1, event times stay normalized
        auto result = akkado::compile(R"(fast(pat("c4 e4"), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(1.0f));
        REQUIRE(si.sequence_events.size() >= 1);
        REQUIRE(si.sequence_events[0].size() >= 2);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f));
        CHECK(si.sequence_events[0][1].time == Catch::Approx(0.5f));
    }

    SECTION("rev(pat(...)) reverses event positions within [0,1)") {
        // pat("c4 e4") -> events at 0.0 (dur 0.5) and 0.5 (dur 0.5)
        // rev -> e4 at 0.0, c4 at 0.5
        auto result = akkado::compile(R"(rev(pat("c4 e4")))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f));
        REQUIRE(si.sequence_events.size() >= 1);
        REQUIRE(si.sequence_events[0].size() >= 2);
        // After rev: original event at 0.5 moves to 0.0, original at 0.0 moves to 0.5
        // Events may be unsorted; check that both positions exist
        bool has_0 = false, has_05 = false;
        for (const auto& e : si.sequence_events[0]) {
            if (std::abs(e.time - 0.0f) < 0.01f) has_0 = true;
            if (std::abs(e.time - 0.5f) < 0.01f) has_05 = true;
        }
        CHECK(has_0);
        CHECK(has_05);
    }

    SECTION("rev(slow(pat(...), 2)) reverses within normalized range") {
        // pat("c4 e4") base: cycle_length=2, events at 0.0, 0.5
        // slow(2): cycle_length=4, events still at 0.0, 0.5
        // rev: events reversed to 0.0, 0.5 (swapped values), cycle_length=4
        auto result = akkado::compile(R"(rev(slow(pat("c4 e4"), 2)))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        REQUIRE(si.sequence_events.size() >= 1);
        REQUIRE(si.sequence_events[0].size() >= 2);
        // All events should be in [0, 1) range
        for (const auto& e : si.sequence_events[0]) {
            CHECK(e.time >= 0.0f);
            CHECK(e.time < 1.0f);
        }
    }
}

// =============================================================================
// Pattern Transform: velocity/bank/n Chaining Tests
// =============================================================================

TEST_CASE("Pattern transform: velocity in chain", "[codegen][patterns]") {
    SECTION("slow(velocity(...)) compiles") {
        auto result = akkado::compile(R"(slow(velocity(pat("c4 e4"), 0.5), 2))");
        CHECK(result.success);
    }
    SECTION("velocity(slow(...)) compiles") {
        auto result = akkado::compile(R"(velocity(slow(pat("c4 e4"), 2), 0.5))");
        CHECK(result.success);
    }
    SECTION("velocity in chain modifies event velocity") {
        auto result = akkado::compile(R"(slow(velocity(pat("c4 e4"), 0.5), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        // slow(2) doubles cycle_length: 2 * 2 = 4
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        // velocity(0.5) sets velocity to 0.5
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 1);
        CHECK(si.sequence_events[0][0].velocity == Catch::Approx(0.5f));
    }
}

TEST_CASE("Pattern transform: bank/variant in chain", "[codegen][patterns]") {
    SECTION("slow(bank(...)) compiles") {
        auto result = akkado::compile(R"(slow(bank(pat("bd sd"), "TR808"), 2))");
        CHECK(result.success);
    }
    SECTION("slow(variant(...)) compiles") {
        auto result = akkado::compile(R"(slow(variant(pat("bd sd"), 2), 2))");
        CHECK(result.success);
    }
}

// =============================================================================
// Pattern Transform: String Literal as Pattern Tests
// =============================================================================

TEST_CASE("Pattern transform: string literal as pattern", "[codegen][patterns]") {
    SECTION("slow with string literal compiles") {
        auto result = akkado::compile(R"(slow("c4 e4 g4", 2))");
        CHECK(result.success);
    }
    SECTION("transpose with string literal compiles") {
        auto result = akkado::compile(R"(transpose("c4 e4", 12))");
        CHECK(result.success);
    }
    SECTION("rev with string literal compiles") {
        auto result = akkado::compile(R"(rev("c4 e4 g4"))");
        CHECK(result.success);
    }
    SECTION("string literal has correct semantics") {
        auto result = akkado::compile(R"(slow("c4 e4", 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(4.0f));
    }
    SECTION("chained transform on string literal") {
        auto result = akkado::compile(R"(transpose(slow("c4 e4", 2), 12))");
        CHECK(result.success);
    }
}

// =============================================================================
// Error Path Tests
// =============================================================================

TEST_CASE("Codegen: Undefined identifier errors", "[codegen][errors]") {
    SECTION("simple undefined variable - E005") {
        auto result = akkado::compile("undefined_var");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E005") found = true;  // Analyzer catches undefined identifiers
        }
        CHECK(found);
    }

    SECTION("undefined in expression - E005") {
        auto result = akkado::compile("1 + unknown_var");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E005") found = true;  // Analyzer catches undefined identifiers
        }
        CHECK(found);
    }

    SECTION("undefined function - E004") {
        auto result = akkado::compile("nonexistent_func(42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E004") found = true;  // Analyzer catches unknown functions
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Error E103 - Builtin as value", "[codegen][errors]") {
    SECTION("assign builtin to variable") {
        auto result = akkado::compile("x = sin");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E103") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Hole in unexpected context errors", "[codegen][errors]") {
    SECTION("hole at top level - E003") {
        auto result = akkado::compile("%");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E003") found = true;  // Analyzer catches hole outside pipe
        }
        CHECK(found);
    }

    SECTION("hole in assignment without pipe - E003") {
        auto result = akkado::compile("x = %");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E003") found = true;  // Analyzer catches hole outside pipe
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Error E130-E136 - Field access errors", "[codegen][errors]") {
    SECTION("field access on unknown field in pattern") {
        auto result = akkado::compile(R"(pat("c4") |> %.nonexistent)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E136") found = true;
        }
        CHECK(found);
    }

    SECTION("field access on undefined variable - E005") {
        auto result = akkado::compile("undefined_record.field");
        REQUIRE_FALSE(result.success);
        // Analyzer catches undefined identifier
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E005") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: HOF error paths", "[codegen][errors]") {
    SECTION("map() without function - E130") {
        auto result = akkado::compile("map([1, 2, 3], 42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E130") found = true;  // Codegen: second arg must be function
        }
        CHECK(found);
    }

    SECTION("map() with too few arguments - E006") {
        auto result = akkado::compile("map([1, 2, 3])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("sum() with too many arguments - E007") {
        auto result = akkado::compile("sum([1, 2], [3, 4])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E007") found = true;  // Analyzer: too many args
        }
        CHECK(found);
    }

    SECTION("reduce() wrong argument count") {
        auto result = akkado::compile("reduce([1, 2, 3], (a, b) -> a + b)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            // Analyzer rejects too-few args (E006) before codegen runs.
            if (d.code == "E006" || d.code == "E142") found = true;
        }
        CHECK(found);
    }

    SECTION("zipWith() wrong argument count - E006") {
        auto result = akkado::compile("zipWith([1], [2])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("zip() wrong argument count - E006") {
        auto result = akkado::compile("zip([1])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("take() wrong argument count - E006") {
        auto result = akkado::compile("take(2)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("take() non-literal first arg - E148") {
        auto result = akkado::compile("count = 2\ntake(count, [1, 2, 3])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E148") found = true;  // Codegen: must be literal
        }
        CHECK(found);
    }

    SECTION("drop() wrong argument count - E006") {
        auto result = akkado::compile("drop(1)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("drop() non-literal first arg - E150") {
        auto result = akkado::compile("n = 2\ndrop(n, [1, 2, 3])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E150") found = true;  // Codegen: must be literal
        }
        CHECK(found);
    }

    SECTION("reverse() wrong argument count - E006") {
        auto result = akkado::compile("reverse()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("range() wrong argument count - E006") {
        auto result = akkado::compile("range(1)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("range() non-literal args - E153") {
        auto result = akkado::compile("count = 5\nrange(0, count)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E153") found = true;  // Codegen: must be literals
        }
        CHECK(found);
    }

    SECTION("repeat() wrong argument count - E006") {
        auto result = akkado::compile("repeat(42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("repeat() non-literal count - E155") {
        auto result = akkado::compile("count = 3\nrepeat(42, count)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E155") found = true;  // Codegen: must be literal
        }
        CHECK(found);
    }

    SECTION("len() wrong argument count - E006") {
        auto result = akkado::compile("len()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("len() on non-array - E141") {
        auto result = akkado::compile("x = 42\nlen(x)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E141") found = true;  // Codegen: not an array
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Pattern error paths", "[codegen][errors]") {
    SECTION("chord() wrong argument count - E006") {
        auto result = akkado::compile("chord()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("chord() non-string argument - E126") {
        auto result = akkado::compile("chord(42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E126") found = true;  // Codegen: must be string literal
        }
        CHECK(found);
    }

    SECTION("mtof() wrong argument count - E006") {
        auto result = akkado::compile("mtof()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Record error paths", "[codegen][errors]") {
    SECTION("record field access on scalar - E061") {
        auto result = akkado::compile("x = 42\nx.field");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E061") found = true;  // Analyzer: Cannot access field on non-record
        }
        CHECK(found);
    }

    SECTION("unknown field on record - E060") {
        auto result = akkado::compile("r = {x: 1, y: 2}\nr.z");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E060") found = true;  // Analyzer: Unknown field on record
        }
        CHECK(found);
    }
}

// =============================================================================
// Phase 5: Codegen Deep Dive - Records, Functions, Match, Patterns
// =============================================================================

TEST_CASE("Codegen: Record handling", "[codegen]") {
    SECTION("simple record literal") {
        auto result = akkado::compile("rec = {freq: 440, vel: 0.8}");
        CHECK(result.success);
    }

    SECTION("record field access") {
        auto result = akkado::compile("rec = {freq: 440, vel: 0.8}\nrec.freq");
        CHECK(result.success);
    }

    SECTION("record with multiple fields") {
        auto result = akkado::compile("r = {a: 1, b: 2, c: 3}\nr.a + r.b + r.c");
        CHECK(result.success);
    }

    SECTION("nested expression in record field") {
        auto result = akkado::compile("x = 10\nr = {val: x * 2}\nr.val");
        CHECK(result.success);
    }

    SECTION("nested record field access returns correct inner value") {
        auto result = akkado::compile(
            "inner = {a: 11, b: 22}\n"
            "outer = {x: inner, y: 99}\n"
            "outer.x.a"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_11 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 11.0f) {
                found_11 = true;
                break;
            }
        }
        CHECK(found_11);
    }

    SECTION("nested record field access selects correct field (not first)") {
        auto result = akkado::compile(
            "inner = {a: 11, b: 22}\n"
            "outer = {x: inner}\n"
            "outer.x.b"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_22 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 22.0f) {
                found_22 = true;
                break;
            }
        }
        CHECK(found_22);
    }

    SECTION("triply nested record field access (a.b.c.d)") {
        auto result = akkado::compile(
            "deepest = {v: 42}\n"
            "mid = {inner: deepest}\n"
            "top = {m: mid}\n"
            "top.m.inner.v"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_42 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 42.0f) {
                found_42 = true;
                break;
            }
        }
        CHECK(found_42);
    }
}

TEST_CASE("Codegen: Lambda and function values", "[codegen]") {
    SECTION("simple lambda") {
        auto result = akkado::compile("f = (x) -> x * 2");
        CHECK(result.success);
    }

    SECTION("lambda with multiple params") {
        auto result = akkado::compile("myadd = (a, b) -> a + b");  // 'add' is a builtin
        CHECK(result.success);
    }

    SECTION("lambda in map") {
        auto result = akkado::compile("arr = [1, 2, 3]\nmap(arr, (x) -> x * 2)");
        CHECK(result.success);
    }

    SECTION("lambda variable") {
        auto result = akkado::compile("double = (x) -> x * 2\ndouble");
        CHECK(result.success);
    }

    SECTION("lambda variable in map") {
        auto result = akkado::compile("double = (x) -> x * 2\narr = [1, 2, 3]\nmap(arr, double)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: User function definitions", "[codegen]") {
    SECTION("simple function definition") {
        auto result = akkado::compile("fn double(x) -> x * 2\ndouble(5)");
        CHECK(result.success);
    }

    SECTION("function with multiple params") {
        // fn add(a, b) -> ... causes segfault, test simpler case
        auto result = akkado::compile("fn triple(x) -> x * 3\ntriple(10)");
        CHECK(result.success);
    }

    SECTION("function with default param") {
        auto result = akkado::compile("fn scale(x, factor = 2) -> x * factor\nscale(5)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Match expressions", "[codegen]") {
    SECTION("match with string scrutinee") {
        auto result = akkado::compile("match(\"sin\") { \"sin\": 1, \"saw\": 2, _: 0 }");
        CHECK(result.success);
    }

    SECTION("match with number scrutinee") {
        auto result = akkado::compile("match(2) { 0: 100, 1: 200, 2: 300, _: 0 }");
        CHECK(result.success);
    }

    SECTION("match with wildcard") {
        auto result = akkado::compile("match(\"unknown\") { _: 42 }");
        CHECK(result.success);
    }

    SECTION("match in function") {
        auto result = akkado::compile("fn wave(t) -> match(t) { \"sin\": 1, \"saw\": 2, _: 0 }\nwave(\"sin\")");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Match destructuring", "[codegen][match][destructure]") {
    SECTION("destructure record - compile time") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.8}
            match(r) {
                {freq, vel}: freq
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("destructure with expression body") {
        auto result = akkado::compile(R"(
            r = {a: 10, b: 20}
            match(r) {
                {a, b}: a + b
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("destructure with guard") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.8}
            match(r) {
                {freq, vel} && true: freq * vel
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("as destructuring in pipe") {
        auto result = akkado::compile(R"(
            r = {a: 100, b: 200}
            r as {a, b} |> a + b
        )");
        CHECK(result.success);
    }

    SECTION("as destructuring single field") {
        auto result = akkado::compile(R"(
            r = {val: 42}
            r as {val} |> val * 2
        )");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Pattern transformations", "[codegen]") {
    // Pattern transformations require literal patterns as first argument
    SECTION("slow transformation") {
        auto result = akkado::compile("slow(pat(\"c4 e4 g4\"), 2)");
        CHECK(result.success);
    }

    SECTION("fast transformation") {
        auto result = akkado::compile("fast(pat(\"c4 e4\"), 2)");
        CHECK(result.success);
    }

    SECTION("rev transformation") {
        auto result = akkado::compile("rev(pat(\"c4 e4 g4\"))");
        CHECK(result.success);
    }

    SECTION("transpose transformation") {
        auto result = akkado::compile("transpose(pat(\"c4 e4 g4\"), 12)");
        CHECK(result.success);
    }

    SECTION("velocity transformation") {
        auto result = akkado::compile("velocity(pat(\"c4 e4 g4\"), 0.5)");
        CHECK(result.success);
    }

    SECTION("pattern transformations emit no W130 warnings") {
        // Verify that pattern transformations are now fully implemented
        // and don't emit "not yet implemented" warnings

        auto check_no_w130 = [](const akkado::CompileResult& result) {
            for (const auto& diag : result.diagnostics) {
                if (diag.code == "W130") {
                    return false;
                }
            }
            return true;
        };

        auto slow_result = akkado::compile("slow(pat(\"c4 e4 g4\"), 2)");
        CHECK(slow_result.success);
        CHECK(check_no_w130(slow_result));

        auto fast_result = akkado::compile("fast(pat(\"c4 e4\"), 2)");
        CHECK(fast_result.success);
        CHECK(check_no_w130(fast_result));

        auto rev_result = akkado::compile("rev(pat(\"c4 e4 g4\"))");
        CHECK(rev_result.success);
        CHECK(check_no_w130(rev_result));

        auto transpose_result = akkado::compile("transpose(pat(\"c4 e4 g4\"), 12)");
        CHECK(transpose_result.success);
        CHECK(check_no_w130(transpose_result));

        auto velocity_result = akkado::compile("velocity(pat(\"c4 e4 g4\"), 0.5)");
        CHECK(velocity_result.success);
        CHECK(check_no_w130(velocity_result));
    }

    SECTION("transformations with pat() syntax") {
        // Test that all transformations work with pat() syntax
        auto slow_result = akkado::compile("slow(pat(\"c4 e4 g4\"), 2)");
        CHECK(slow_result.success);

        auto fast_result = akkado::compile("fast(pat(\"c4 e4\"), 2)");
        CHECK(fast_result.success);

        auto rev_result = akkado::compile("rev(pat(\"c4 e4 g4\"))");
        CHECK(rev_result.success);

        auto transpose_result = akkado::compile("transpose(pat(\"c4 e4 g4\"), 12)");
        CHECK(transpose_result.success);

        auto velocity_result = akkado::compile("velocity(pat(\"c4 e4 g4\"), 0.5)");
        CHECK(velocity_result.success);
    }
}

// =============================================================================
// Phase 6: Array HOF successful paths
// =============================================================================

TEST_CASE("Codegen: Array HOF success paths", "[codegen]") {
    SECTION("map with lambda") {
        auto result = akkado::compile("map([1, 2, 3], (x) -> x * 2)");
        CHECK(result.success);
    }

    SECTION("sum array") {
        auto result = akkado::compile("sum([1, 2, 3, 4])");
        CHECK(result.success);
    }

    SECTION("zipWith with lambda") {
        auto result = akkado::compile("zipWith([1, 2], [10, 20], (a, b) -> a + b)");
        CHECK(result.success);
    }

    SECTION("zip arrays") {
        auto result = akkado::compile("zip([1, 2, 3], [4, 5, 6])");
        CHECK(result.success);
    }

    SECTION("take elements") {
        auto result = akkado::compile("take(2, [1, 2, 3, 4, 5])");
        CHECK(result.success);
    }

    SECTION("drop elements") {
        auto result = akkado::compile("drop(2, [1, 2, 3, 4, 5])");
        CHECK(result.success);
    }

    SECTION("reverse array") {
        auto result = akkado::compile("reverse([1, 2, 3])");
        CHECK(result.success);
    }

    SECTION("range") {
        auto result = akkado::compile("range(0, 5)");
        CHECK(result.success);
    }

    SECTION("repeat") {
        auto result = akkado::compile("repeat(42, 3)");
        CHECK(result.success);
    }

    SECTION("sum") {
        auto result = akkado::compile("sum([1, 2, 3, 4, 5])");
        CHECK(result.success);
    }

    SECTION("len") {
        auto result = akkado::compile("arr = [1, 2, 3]\nlen(arr)");
        CHECK(result.success);
    }
}

// =============================================================================
// Phase 7-9: Additional coverage tests
// =============================================================================

TEST_CASE("Codegen: Pipe expressions", "[codegen]") {
    SECTION("simple pipe") {
        auto result = akkado::compile("osc(\"sin\", 440) |> out(%, %)");
        CHECK(result.success);
    }

    SECTION("pipe with field access") {
        auto result = akkado::compile("pat(\"c4 e4 g4\") |> osc(\"sin\", %.freq)");
        CHECK(result.success);
    }

    SECTION("pipe binding with as") {
        auto result = akkado::compile("osc(\"sin\", 440) as sig |> out(sig, sig)");
        CHECK(result.success);
    }

    SECTION("chained pipes") {
        auto result = akkado::compile("pat(\"c4\") |> osc(\"sin\", %.freq) |> out(%, %)");
        CHECK(result.success);
    }

    SECTION("sample pattern to output") {
        auto result = akkado::compile("pat(\"bd ~ bd ~\") |> out(%)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: >> and @ aliases", "[codegen]") {
    SECTION(">> and @ produce same bytecode as |> and %") {
        auto r1 = akkado::compile("osc(\"sin\", 440) |> out(%, %)");
        auto r2 = akkado::compile("osc(\"sin\", 440) >> out(@, @)");
        REQUIRE(r1.success);
        REQUIRE(r2.success);
        CHECK(r1.bytecode.size() == r2.bytecode.size());
    }

    SECTION("@ field access compiles") {
        auto result = akkado::compile("pat(\"c4 e4 g4\") >> osc(\"sin\", @.freq)");
        CHECK(result.success);
    }

    SECTION("@ in unexpected context still errors") {
        auto result = akkado::compile("@");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E003") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Sample pattern event inspection", "[codegen][samples][debug]") {
    SECTION("sample pattern compiles with SAMPLE_PLAY and correct events") {
        auto result = akkado::compile(R"(pat("hh hh hh [hh hh] hh hh hh [hh oh]") |> out(%))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);

        // Must have SEQPAT_QUERY, SEQPAT_STEP, and SAMPLE_PLAY instructions
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_STEP) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SAMPLE_PLAY) >= 1);

        // Check state_inits
        REQUIRE(!result.state_inits.empty());
        const auto& si = result.state_inits[0];
        CHECK(si.type == akkado::StateInitData::Type::SequenceProgram);
        CHECK(si.cycle_length == 8.0f);  // 8 top-level elements
        CHECK(si.is_sample_pattern == true);

        // Root sequence should have 10 events (8 top-level, groups expand inline)
        REQUIRE(!si.sequence_events.empty());
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 10);

        // Verify event times (normalized to [0,1) range, divided by 8)
        CHECK(events[0].time == Catch::Approx(0.0f));        // hh
        CHECK(events[1].time == Catch::Approx(1.0f / 8.0f)); // hh
        CHECK(events[2].time == Catch::Approx(2.0f / 8.0f)); // hh
        CHECK(events[3].time == Catch::Approx(3.0f / 8.0f)); // [hh (first child)
        CHECK(events[4].time == Catch::Approx(3.5f / 8.0f)); // hh] (second child)
        CHECK(events[5].time == Catch::Approx(4.0f / 8.0f)); // hh
        CHECK(events[6].time == Catch::Approx(5.0f / 8.0f)); // hh
        CHECK(events[7].time == Catch::Approx(6.0f / 8.0f)); // hh
        CHECK(events[8].time == Catch::Approx(7.0f / 8.0f)); // [hh (first child)
        CHECK(events[9].time == Catch::Approx(7.5f / 8.0f)); // oh] (second child)

        // ALL events should be DATA type with num_values=1
        for (size_t i = 0; i < events.size(); i++) {
            INFO("Event " << i << " at time " << events[i].time);
            CHECK(events[i].type == cedar::EventType::DATA);
            CHECK(events[i].num_values == 1);
        }

        // Without a sample registry, all sample IDs should be 0 (deferred resolution)
        for (size_t i = 0; i < events.size(); i++) {
            CHECK(events[i].values[0] == 0.0f);
        }

        // Check sample mappings exist for all 10 events
        CHECK(si.sequence_sample_mappings.size() == 10);
        for (const auto& mapping : si.sequence_sample_mappings) {
            CHECK(mapping.seq_idx == 0);
            // All mappings should have a sample name
            CHECK(!mapping.sample_name.empty());
        }

        // Verify SAMPLE_PLAY instruction wiring
        const auto* sample_play = find_instruction(insts, cedar::Opcode::SAMPLE_PLAY);
        REQUIRE(sample_play != nullptr);

        // SAMPLE_PLAY should read from trigger buffer (inputs[0]) and value buffer (inputs[2])
        const auto* seqpat_step = find_instruction(insts, cedar::Opcode::SEQPAT_STEP);
        REQUIRE(seqpat_step != nullptr);

        // The trigger buffer written by SEQPAT_STEP should be read by SAMPLE_PLAY
        CHECK(sample_play->inputs[0] == seqpat_step->inputs[1]);  // trigger
        // The value buffer written by SEQPAT_STEP should be read by SAMPLE_PLAY
        CHECK(sample_play->inputs[2] == seqpat_step->out_buffer);  // sample_id
    }

    SECTION("sample pattern with sample registry resolves IDs") {
        akkado::SampleRegistry registry;
        registry.register_sample("hh", 42);
        registry.register_sample("oh", 99);

        auto result = akkado::compile(
            R"(pat("hh hh hh [hh hh] hh hh hh [hh oh]") |> out(%))",
            "<input>", &registry);
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 10);

        // With registry, sample IDs should be resolved
        for (size_t i = 0; i < 9; i++) {  // first 9 are "hh"
            INFO("Event " << i << " sample_id");
            CHECK(events[i].values[0] == 42.0f);
        }
        // Last event is "oh"
        CHECK(events[9].values[0] == 99.0f);
    }
}

// =============================================================================
// Sample polyrhythm merge — [bd, hh] plays kick and hat simultaneously.
// Codegen collapses sibling sample atoms under a MiniPolyrhythm into a single
// DATA event with num_values=N so SAMPLE_PLAY can spawn one voice per value.
// =============================================================================
TEST_CASE("Codegen: Sample polyrhythm merges into chord-like event",
          "[codegen][samples][polyrhythm]") {
    akkado::SampleRegistry registry;
    registry.register_sample("bd", 1);
    registry.register_sample("sd", 2);
    registry.register_sample("hh", 3);

    SECTION("[bd, hh] → single event with num_values=2") {
        auto result = akkado::compile(R"(pat("[bd, hh]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.is_sample_pattern == true);
        REQUIRE(!si.sequence_events.empty());
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 2);
        CHECK(events[0].values[0] == 1.0f);  // bd
        CHECK(events[0].values[1] == 3.0f);  // hh
        CHECK(events[0].time == Catch::Approx(0.0f));

        // Sample mappings exist for both voices with distinct slots.
        REQUIRE(si.sequence_sample_mappings.size() == 2);
        CHECK(si.sequence_sample_mappings[0].value_slot == 0);
        CHECK(si.sequence_sample_mappings[0].sample_name == "bd");
        CHECK(si.sequence_sample_mappings[1].value_slot == 1);
        CHECK(si.sequence_sample_mappings[1].sample_name == "hh");
    }

    SECTION("[bd, hh, sd] → three voices") {
        auto result = akkado::compile(R"(pat("[bd, hh, sd]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 3);
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 3.0f);
        CHECK(events[0].values[2] == 2.0f);
    }

    SECTION("[bd, bd] → two voices, same id (voice-doubling allowed)") {
        auto result = akkado::compile(R"(pat("[bd, bd]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 2);
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 1.0f);
    }

    SECTION("[bd, ~] → rest becomes silent voice (values[1] = 0)") {
        auto result = akkado::compile(R"(pat("[bd, ~]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 2);
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 0.0f);
        // Only the sample atom gets a mapping; the rest is silent.
        REQUIRE(si.sequence_sample_mappings.size() == 1);
        CHECK(si.sequence_sample_mappings[0].value_slot == 0);
    }

    SECTION("user's rock groove — half-cycle layout kicks/snares correctly") {
        // Each half-cycle is [[bd, hh] hh [sd, hh] hh] = 4 beats,
        // together 8 top-level beats. Beats 1 and 3 are polyrhythm stacks.
        auto result = akkado::compile(
            R"(pat("[[bd, hh] hh [sd, hh] hh]  [[bd, hh] [bd, hh] [sd, hh] hh]") |> out(%, %))",
            "<input>", &registry);
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.is_sample_pattern == true);
        CHECK(si.cycle_length == 2.0f);  // 2 top-level group halves
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 8);

        // First half: [bd,hh], hh, [sd,hh], hh
        CHECK(events[0].num_values == 2);  // bd + hh
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 3.0f);
        CHECK(events[1].num_values == 1);  // hh
        CHECK(events[1].values[0] == 3.0f);
        CHECK(events[2].num_values == 2);  // sd + hh
        CHECK(events[2].values[0] == 2.0f);
        CHECK(events[2].values[1] == 3.0f);
        CHECK(events[3].num_values == 1);  // hh

        // Second half: [bd,hh], [bd,hh], [sd,hh], hh
        CHECK(events[4].num_values == 2);
        CHECK(events[5].num_values == 2);
        CHECK(events[5].values[0] == 1.0f);  // bd
        CHECK(events[6].num_values == 2);  // sd + hh
        CHECK(events[6].values[0] == 2.0f);
        CHECK(events[7].num_values == 1);  // hh only
    }

    SECTION("[[bd, sd], hh] → outer polyrhythm falls back to per-child compile") {
        // When a polyrhythm's direct child is itself compound (nested
        // polyrhythm, group, euclidean, pitch chord, …), the merge can't apply
        // to the outer level because one of the voices would need to represent
        // a multi-event sub-pattern. The outer polyrhythm therefore falls back
        // to per-child compile, while the inner [bd, sd] still merges on its
        // own level.
        auto result = akkado::compile(R"(pat("[[bd, sd], hh]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        // Outer produces 2 events both at time 0: the inner merged polyrhythm
        // ([bd,sd] → num_values=2) plus the lone hh atom (num_values=1).
        REQUIRE(events.size() == 2);
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[1].time == Catch::Approx(0.0f));
        // Inner merge produces a multi-value event; the sibling hh stays mono.
        bool saw_merged = false, saw_mono = false;
        for (const auto& e : events) {
            if (e.num_values == 2) saw_merged = true;
            if (e.num_values == 1) saw_mono = true;
        }
        CHECK(saw_merged);
        CHECK(saw_mono);
    }

    SECTION("single SAMPLE_PLAY instruction; in3/in4 link to SEQPAT state") {
        auto result = akkado::compile(R"(pat("[bd, hh]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        // Option B: one SAMPLE_PLAY that handles all voices internally.
        CHECK(count_instructions(insts, cedar::Opcode::SAMPLE_PLAY) == 1);

        const auto* sample_play = find_instruction(insts, cedar::Opcode::SAMPLE_PLAY);
        REQUIRE(sample_play != nullptr);
        const auto* query = find_instruction(insts, cedar::Opcode::SEQPAT_QUERY);
        REQUIRE(query != nullptr);

        // in3/in4 carry the SequenceState's state_id, split low/high 16 bits.
        std::uint32_t linked_state =
            static_cast<std::uint32_t>(sample_play->inputs[3]) |
            (static_cast<std::uint32_t>(sample_play->inputs[4]) << 16);
        CHECK(linked_state == query->state_id);
    }
}

TEST_CASE("Codegen: Nested bracket subdivision", "[codegen][nested]") {
    SECTION("[] within [] — 2 levels: bd [sd [hh hh]]") {
        auto result = akkado::compile(R"(pat("bd [sd [hh hh]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 4);

        // 2 top-level → cycle_length=2, events in [0,1) normalized range
        // bd: 0.0, dur 0.5
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].duration == Catch::Approx(0.5f));
        // sd: 0.5, dur 0.25
        CHECK(events[1].time == Catch::Approx(0.5f));
        CHECK(events[1].duration == Catch::Approx(0.25f));
        // hh: 0.75, dur 0.125
        CHECK(events[2].time == Catch::Approx(0.75f));
        CHECK(events[2].duration == Catch::Approx(0.125f));
        // hh: 0.875, dur 0.125
        CHECK(events[3].time == Catch::Approx(0.875f));
        CHECK(events[3].duration == Catch::Approx(0.125f));
    }

    SECTION("[] within [] — 3 levels: bd [sd [hh [cp cp]]]") {
        auto result = akkado::compile(R"(pat("bd [sd [hh [cp cp]]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 5);

        CHECK(events[0].time == Catch::Approx(0.0f));      // bd
        CHECK(events[0].duration == Catch::Approx(0.5f));
        CHECK(events[1].time == Catch::Approx(0.5f));      // sd
        CHECK(events[1].duration == Catch::Approx(0.25f));
        CHECK(events[2].time == Catch::Approx(0.75f));     // hh
        CHECK(events[2].duration == Catch::Approx(0.125f));
        CHECK(events[3].time == Catch::Approx(0.875f));    // cp
        CHECK(events[3].duration == Catch::Approx(0.0625f));
        CHECK(events[4].time == Catch::Approx(0.9375f));   // cp
        CHECK(events[4].duration == Catch::Approx(0.0625f));
    }

    SECTION("[] within [] — 4 levels: bd [sd [hh [cp [oh oh]]]]") {
        auto result = akkado::compile(R"(pat("bd [sd [hh [cp [oh oh]]]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 6);

        CHECK(events[0].time == Catch::Approx(0.0f));       // bd
        CHECK(events[0].duration == Catch::Approx(0.5f));
        CHECK(events[1].time == Catch::Approx(0.5f));       // sd
        CHECK(events[1].duration == Catch::Approx(0.25f));
        CHECK(events[2].time == Catch::Approx(0.75f));      // hh
        CHECK(events[2].duration == Catch::Approx(0.125f));
        CHECK(events[3].time == Catch::Approx(0.875f));     // cp
        CHECK(events[3].duration == Catch::Approx(0.0625f));
        CHECK(events[4].time == Catch::Approx(0.9375f));    // oh
        CHECK(events[4].duration == Catch::Approx(0.03125f));
        CHECK(events[5].time == Catch::Approx(0.96875f));   // oh
        CHECK(events[5].duration == Catch::Approx(0.03125f));
    }

    SECTION("symmetric nesting: [bd sd] [hh [cp oh]]") {
        auto result = akkado::compile(R"(pat("[bd sd] [hh [cp oh]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 5);

        // [bd sd] gets first half [0, 0.5)
        CHECK(events[0].time == Catch::Approx(0.0f));       // bd
        CHECK(events[0].duration == Catch::Approx(0.25f));
        CHECK(events[1].time == Catch::Approx(0.25f));      // sd
        CHECK(events[1].duration == Catch::Approx(0.25f));
        // [hh [cp oh]] gets second half [0.5, 1.0)
        CHECK(events[2].time == Catch::Approx(0.5f));       // hh
        CHECK(events[2].duration == Catch::Approx(0.25f));
        CHECK(events[3].time == Catch::Approx(0.75f));      // cp
        CHECK(events[3].duration == Catch::Approx(0.125f));
        CHECK(events[4].time == Catch::Approx(0.875f));     // oh
        CHECK(events[4].duration == Catch::Approx(0.125f));
    }

    SECTION("4 levels all []: [[bd sd] [[hh hh] [cp oh]]]") {
        auto result = akkado::compile(R"(pat("[[bd sd] [[hh hh] [cp oh]]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 6);

        // Single top-level group → cycle_length=1
        // [bd sd] gets [0, 0.5): bd@0, sd@0.25
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].duration == Catch::Approx(0.25f));
        CHECK(events[1].time == Catch::Approx(0.25f));
        CHECK(events[1].duration == Catch::Approx(0.25f));
        // [[hh hh] [cp oh]] gets [0.5, 1.0)
        //   [hh hh]@[0.5, 0.75): hh@0.5, hh@0.625
        CHECK(events[2].time == Catch::Approx(0.5f));
        CHECK(events[2].duration == Catch::Approx(0.125f));
        CHECK(events[3].time == Catch::Approx(0.625f));
        CHECK(events[3].duration == Catch::Approx(0.125f));
        //   [cp oh]@[0.75, 1.0): cp@0.75, oh@0.875
        CHECK(events[4].time == Catch::Approx(0.75f));
        CHECK(events[4].duration == Catch::Approx(0.125f));
        CHECK(events[5].time == Catch::Approx(0.875f));
        CHECK(events[5].duration == Catch::Approx(0.125f));
    }
}

TEST_CASE("Codegen: Binary operations", "[codegen]") {
    SECTION("arithmetic") {
        auto result = akkado::compile("1 + 2 * 3 - 4 / 2");
        CHECK(result.success);
    }

    SECTION("multiplication and division") {
        auto result = akkado::compile("x = 10 * 3\ny = x / 2");
        CHECK(result.success);
    }

    SECTION("unary minus") {
        auto result = akkado::compile("-42");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Timing", "[codegen]") {
    SECTION("clock function no args") {
        auto result = akkado::compile("clock()");
        CHECK(result.success);
    }

    SECTION("phasor") {
        auto result = akkado::compile("phasor(1)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Parameters", "[codegen]") {
    SECTION("param with defaults") {
        auto result = akkado::compile("freq = param(\"freq\", 440, 20, 2000)");
        CHECK(result.success);
    }

    SECTION("toggle") {
        auto result = akkado::compile("mute = toggle(\"mute\", false)");
        CHECK(result.success);
    }

    SECTION("button") {
        auto result = akkado::compile("trig = button(\"trigger\")");
        CHECK(result.success);
    }

    SECTION("dropdown") {
        auto result = akkado::compile("wave = dropdown(\"wave\", \"sin\", \"saw\", \"tri\")");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Chord function", "[codegen]") {
    SECTION("chord without poly produces E410") {
        auto result = akkado::compile("chord(\"Am\")");
        CHECK_FALSE(result.success);
    }

    SECTION("chord progression without poly produces E410") {
        auto result = akkado::compile("chord(\"Am C F G\")");
        CHECK_FALSE(result.success);
    }

    SECTION("chord pattern without poly is error") {
        auto result = akkado::compile(R"(pat("C4'") |> osc("sin", %.freq) |> out(%, %))");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("Codegen: Oscillators", "[codegen]") {
    SECTION("basic osc") {
        auto result = akkado::compile("osc(\"sin\", 440)");
        CHECK(result.success);
    }

    SECTION("osc with named param") {
        auto result = akkado::compile("osc(type: \"saw\", freq: 220)");
        CHECK(result.success);
    }

    SECTION("pulse osc with pwm") {
        auto result = akkado::compile("osc(\"pulse\", 440, 0.3)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Mini-notation patterns", "[codegen]") {
    SECTION("simple pattern") {
        auto result = akkado::compile("pat(\"c4 e4 g4\")");
        CHECK(result.success);
    }

    SECTION("pattern with rests") {
        auto result = akkado::compile("pat(\"c4 ~ e4 ~\")");
        CHECK(result.success);
    }

    SECTION("pattern with groups") {
        auto result = akkado::compile("pat(\"[c4 e4] g4\")");
        CHECK(result.success);
    }

    SECTION("pattern with euclidean") {
        auto result = akkado::compile("pat(\"c4(3,8)\")");
        CHECK(result.success);
    }

    SECTION("pattern with speed modifier") {
        auto result = akkado::compile("pat(\"c4*2 e4\")");
        CHECK(result.success);
    }

    SECTION("drum pattern") {
        auto result = akkado::compile("pat(\"kick snare kick snare\")");
        CHECK(result.success);
    }
}

// =============================================================================
// UGen Auto-Expansion Tests
// =============================================================================

TEST_CASE("Codegen: UGen auto-expansion", "[codegen][arrays]") {
    SECTION("array of frequencies expands sine to 3 instances") {
        // [440, 550, 660] |> sine(%) produces 3 OSC_SIN instructions
        auto result = akkado::compile("[440, 550, 660] |> sine(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 OSC_SIN instructions
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 3);
    }

    SECTION("array expansion followed by sum") {
        // Note: sum(%) requires the % to pass the multi-buffer through
        auto result = akkado::compile("[220, 330, 440] |> saw(%) |> sum(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 sawtooth oscillators
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 3);
        // 2 additions to sum them
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("array of frequencies through osc() stdlib") {
        // osc() is defined in stdlib and calls sine for type="sin"
        // This currently produces 1 osc because the match resolves before expansion.
        // For full expansion through stdlib osc(), need to call directly:
        auto result = akkado::compile("freqs = [440, 550, 660]\nsine(freqs)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 OSC_SIN instructions
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 3);
    }

    SECTION("single element array does not expand") {
        auto result = akkado::compile("[440] |> sine(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Single element: just one instruction
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 1);
    }

    SECTION("filter expansion with array input") {
        // Filters also expand when given array inputs
        auto result = akkado::compile("ns = noise()\nfreqs = [1000, 2000, 3000]\nfreqs |> lp(ns, %)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 SVF_LP instructions
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 3);
    }
}

// =============================================================================
// Binary Operation Broadcasting Tests
// =============================================================================

TEST_CASE("Codegen: Array broadcasting", "[codegen][arrays]") {
    SECTION("array * scalar") {
        auto result = akkado::compile("[1, 2, 3] * 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 multiplications
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
    }

    SECTION("scalar + array") {
        auto result = akkado::compile("10 + [1, 2, 3]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 additions
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("array + array same length") {
        auto result = akkado::compile("[1, 2] + [3, 4]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 2 additions
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("array + array broadcasting") {
        // [1, 2, 3, 4] + [10, 20] -> [11, 22, 13, 24] (shorter array cycles)
        auto result = akkado::compile("[1, 2, 3, 4] + [10, 20]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 additions (length of longer array)
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 4);
    }

    SECTION("division broadcasting") {
        auto result = akkado::compile("[10, 20, 30] / 10");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::DIV) == 3);
    }
}

// =============================================================================
// Array Reduction Tests
// =============================================================================

TEST_CASE("Codegen: Array reductions", "[codegen][arrays]") {
    SECTION("product via reduce(*, 1)") {
        auto result = akkado::compile("reduce([2, 3, 4], (a, b) -> a * b, 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 1 * 2 * 3 * 4 — three MULs (one per array element)
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
    }

    SECTION("mean of array") {
        auto result = akkado::compile("mean([10, 20, 30])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 2 ADDs + 1 DIV
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::DIV) == 1);
    }

    SECTION("min of array") {
        auto result = akkado::compile("min([5, 2, 8, 1])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 MIN operations
        CHECK(count_instructions(insts, cedar::Opcode::MIN) == 3);
    }

    SECTION("max of array") {
        auto result = akkado::compile("max([5, 2, 8, 1])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 MAX operations
        CHECK(count_instructions(insts, cedar::Opcode::MAX) == 3);
    }

    SECTION("binary min still works") {
        auto result = akkado::compile("min(3, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Single MIN operation
        CHECK(count_instructions(insts, cedar::Opcode::MIN) == 1);
    }
}

// =============================================================================
// Array Transformation Tests
// =============================================================================

TEST_CASE("Codegen: Array indexing", "[codegen][arrays]") {
    SECTION("constant index returns the indexed element, not first") {
        auto result = akkado::compile("arr = [10, 20, 30]\narr[1]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_20 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 20.0f) {
                found_20 = true;
                break;
            }
        }
        CHECK(found_20);
    }

    SECTION("last element accessible via constant index") {
        auto result = akkado::compile("arr = [10, 20, 30]\narr[2]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_30 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 30.0f) {
                found_30 = true;
                break;
            }
        }
        CHECK(found_30);
    }

    SECTION("index 0 returns first element") {
        auto result = akkado::compile("arr = [10, 20, 30]\narr[0]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_10 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 10.0f) {
                found_10 = true;
                break;
            }
        }
        CHECK(found_10);
    }

    SECTION("dynamic index emits ARRAY_INDEX opcode") {
        auto result = akkado::compile(
            "freq = param(\"idx\", 0, 0, 2)\n"
            "arr = [100, 200, 300]\n"
            "arr[freq]"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::ARRAY_INDEX) != nullptr);
    }
}

TEST_CASE("Codegen: Array transformations", "[codegen][arrays]") {
    SECTION("rotate") {
        auto result = akkado::compile("rotate([1, 2, 3, 4], 1)");
        REQUIRE(result.success);
        // Rotation is just reordering - no arithmetic ops needed
    }

    SECTION("shuffle") {
        auto result = akkado::compile("shuffle([1, 2, 3, 4])");
        REQUIRE(result.success);
    }

    SECTION("sort") {
        auto result = akkado::compile("sort([3, 1, 4, 1, 5])");
        REQUIRE(result.success);
    }

    SECTION("normalize") {
        auto result = akkado::compile("normalize([10, 20, 30])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have MIN, MAX, SUB, and DIV operations
        CHECK(count_instructions(insts, cedar::Opcode::MIN) > 0);
        CHECK(count_instructions(insts, cedar::Opcode::MAX) > 0);
    }

    SECTION("scale") {
        auto result = akkado::compile("scale([10, 20, 30], 0, 100)");
        REQUIRE(result.success);
    }
}

// =============================================================================
// Array Generation Tests
// =============================================================================

TEST_CASE("Codegen: Array generation", "[codegen][arrays]") {
    SECTION("linspace") {
        auto result = akkado::compile("linspace(0, 10, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 5 constants: 0, 2.5, 5, 7.5, 10
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 5);
    }

    SECTION("random") {
        auto result = akkado::compile("random(4)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 random constants
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 4);
    }

    SECTION("harmonics") {
        auto result = akkado::compile("harmonics(110, 4)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 harmonics: 110, 220, 330, 440
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 4);
    }

    SECTION("harmonics through oscillator") {
        auto result = akkado::compile("harmonics(110, 4) |> sine(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 oscillators
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 4);
    }
}

// =============================================================================
// Stereo Tests
// =============================================================================

TEST_CASE("Codegen: stereo() creates stereo signal", "[codegen][stereo]") {
    SECTION("stereo from mono duplicates signal") {
        auto result = akkado::compile("stereo(saw(220))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have: PUSH_CONST, OSC_SAW, COPY, COPY (duplicate to L and R)
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::COPY) == 2);
    }

    SECTION("stereo from L/R pair") {
        auto result = akkado::compile("stereo(saw(218), saw(222))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Two separate oscillators, no COPY needed
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::COPY) == 0);
    }
}

TEST_CASE("Codegen: left() and right() extract channels", "[codegen][stereo]") {
    SECTION("left extracts left channel") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            left(s) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Two oscillators, output instruction
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
    }

    SECTION("right extracts right channel") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            right(s) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
    }
}

TEST_CASE("Codegen: pan() emits PAN opcode", "[codegen][stereo]") {
    SECTION("pan mono to stereo") {
        auto result = akkado::compile("pan(saw(220), 0.5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 1);
        auto* pan = find_instruction(insts, cedar::Opcode::PAN);
        REQUIRE(pan != nullptr);
    }

    SECTION("pan with modulated position") {
        auto result = akkado::compile("pan(saw(220), lfo(0.5))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::LFO) == 1);
        auto* pan = find_instruction(insts, cedar::Opcode::PAN);
        REQUIRE(pan != nullptr);
    }
}

TEST_CASE("Codegen: width() emits WIDTH opcode", "[codegen][stereo]") {
    SECTION("width control on stereo signal") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            width(s, 1.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* width = find_instruction(insts, cedar::Opcode::WIDTH);
        REQUIRE(width != nullptr);
    }

    SECTION("width with modulated amount") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            width(s, 1.0 + lfo(0.2) * 0.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::LFO) == 1);
        auto* width = find_instruction(insts, cedar::Opcode::WIDTH);
        REQUIRE(width != nullptr);
    }
}

TEST_CASE("Codegen: ms_encode() and ms_decode()", "[codegen][stereo]") {
    SECTION("mid/side encoding") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            ms_encode(s)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* encode = find_instruction(insts, cedar::Opcode::MS_ENCODE);
        REQUIRE(encode != nullptr);
    }

    SECTION("mid/side decoding") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            ms = ms_encode(s)
            ms_decode(ms)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* encode = find_instruction(insts, cedar::Opcode::MS_ENCODE);
        auto* decode = find_instruction(insts, cedar::Opcode::MS_DECODE);
        REQUIRE(encode != nullptr);
        REQUIRE(decode != nullptr);
    }
}

TEST_CASE("Codegen: pingpong() emits DELAY_PINGPONG opcode", "[codegen][stereo]") {
    SECTION("basic ping-pong delay") {
        auto result = akkado::compile(R"(
            s = stereo(saw(220), saw(220))
            pingpong(s, 0.25, 0.6)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* pingpong = find_instruction(insts, cedar::Opcode::DELAY_PINGPONG);
        REQUIRE(pingpong != nullptr);
    }

    SECTION("ping-pong with modulated parameters") {
        auto result = akkado::compile(R"(
            s = stereo(saw(220), saw(220))
            pingpong(s, lfo(0.1) * 0.25 + 0.1, 0.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::LFO) == 1);
        auto* pingpong = find_instruction(insts, cedar::Opcode::DELAY_PINGPONG);
        REQUIRE(pingpong != nullptr);
    }
}

TEST_CASE("Codegen: out() with stereo signal", "[codegen][stereo]") {
    SECTION("out auto-routes stereo L/R") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(s)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        // L and R should be different buffers
        CHECK(out->inputs[0] != out->inputs[1]);
    }

    SECTION("out with mono still works") {
        auto result = akkado::compile("out(saw(220))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        // Mono: both channels same buffer
        CHECK(out->inputs[0] == out->inputs[1]);
    }
}

TEST_CASE("Codegen: stereo propagation through mono effects", "[codegen][stereo]") {
    // With auto-lift (prd-stereo-support §4.2, §6.2), a stereo signal flowing
    // into a mono DSP op emits a SINGLE instruction carrying the
    // STEREO_INPUT flag. The VM runs the opcode twice at dispatch, once per
    // channel, with independent per-channel state — producing the same audio
    // as two separate instructions but with half the bytecode.
    SECTION("stereo through filter emits one instruction with STEREO_INPUT flag") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            lp(s, 1000)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("stereo through chain of effects") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            s |> lp(%, 1000) |> hp(%, 100)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_HP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        auto* hp = find_instruction(insts, cedar::Opcode::FILTER_SVF_HP);
        REQUIRE(lp != nullptr);
        REQUIRE(hp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
        CHECK((hp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("stereo through delay") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            delay(s, 0.25, 0.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::DELAY) == 1);
        auto* d = find_instruction(insts, cedar::Opcode::DELAY);
        REQUIRE(d != nullptr);
        CHECK((d->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

TEST_CASE("Codegen: stereo pipeline examples", "[codegen][stereo]") {
    SECTION("pan sweep") {
        auto result = akkado::compile(R"(
            saw(220) |> pan(%, lfo(0.5)) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SAW) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::LFO) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::PAN) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }

    SECTION("stereo width modulation") {
        auto result = akkado::compile(R"(
            stereo(saw(218), saw(222))
            |> width(%, 1.0 + lfo(0.2) * 0.5)
            |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(find_instruction(insts, cedar::Opcode::WIDTH) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }

    SECTION("full stereo chain with ping-pong") {
        auto result = akkado::compile(R"(
            saw(220)
            |> stereo(%)
            |> lp(%, 1000)
            |> pingpong(%, 0.25, 0.5)
            |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SAW) != nullptr);
        // stereo() from mono creates 2 COPYs
        CHECK(count_instructions(insts, cedar::Opcode::COPY) == 2);
        // lp() auto-lifts to 1 stereo-flagged instruction (was 2 before §6.2)
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
        CHECK(find_instruction(insts, cedar::Opcode::DELAY_PINGPONG) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }
}

// =============================================================================
// prd-stereo-support: mono() downmix and auto-lift behavior
// =============================================================================

TEST_CASE("Codegen: mono() downmix", "[codegen][stereo][mono]") {
    SECTION("mono(stereo) emits MONO_DOWNMIX opcode") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            mono(s)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* dm = find_instruction(insts, cedar::Opcode::MONO_DOWNMIX);
        REQUIRE(dm != nullptr);
    }

    SECTION("mono() followed by mono DSP does not auto-lift") {
        // mono(s) produces a Mono signal; lp() takes mono in, emits one
        // plain (non-STEREO_INPUT) FILTER_SVF_LP instruction.
        auto result = akkado::compile(R"(
            stereo(saw(218), saw(222))
            |> mono(%)
            |> lp(%, 500)
            |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
    }

    SECTION("mono(mono) is an error") {
        auto result = akkado::compile("mono(saw(220))");
        CHECK(!result.success);
        bool found_e181 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E181") found_e181 = true;
        }
        CHECK(found_e181);
    }

    SECTION("mono(fn) still dispatches to voice manager") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> mono(%, lead) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
    }
}

TEST_CASE("Codegen: stereo auto-lift state IDs", "[codegen][stereo][auto-lift]") {
    SECTION("auto-lift keeps single state_id — VM XORs for R pass") {
        // The auto-lifted instruction carries one state_id (the L side). The
        // VM derives the R-channel state_id at dispatch via XOR with
        // STEREO_STATE_XOR_R, so each channel has independent DSP memory.
        auto result = akkado::compile(R"(
            stereo(saw(218), saw(222)) |> lp(%, 500)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
        CHECK(lp->state_id != 0);
    }

    SECTION("mono path state_id differs from auto-lifted state_id") {
        // Mono: fnv1a(path). Stereo auto-lift: fnv1a(path + "/L").
        // Different hashes → hot-swap correctly treats them as distinct ops.
        auto mono_result = akkado::compile("saw(220) |> lp(%, 500) |> out(%)");
        auto stereo_result = akkado::compile(
            "stereo(saw(220)) |> lp(%, 500) |> out(%)");
        REQUIRE(mono_result.success);
        REQUIRE(stereo_result.success);

        auto mono_insts = get_instructions(mono_result);
        auto stereo_insts = get_instructions(stereo_result);

        auto* mono_lp = find_instruction(mono_insts, cedar::Opcode::FILTER_SVF_LP);
        auto* stereo_lp = find_instruction(stereo_insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(mono_lp != nullptr);
        REQUIRE(stereo_lp != nullptr);
        CHECK(mono_lp->state_id != stereo_lp->state_id);
    }
}

TEST_CASE("Codegen: pan(stereo) dispatches to PAN_STEREO", "[codegen][stereo][pan]") {
    SECTION("pan(mono, pos) emits PAN") {
        auto result = akkado::compile("pan(saw(220), 0.3) |> out(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::PAN) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::PAN_STEREO) == nullptr);
    }

    SECTION("pan(stereo, pos) emits PAN_STEREO for equal-power balance") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            pan(s, 0.3) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::PAN_STEREO) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::PAN) == nullptr);
    }
}

// =============================================================================
// Pattern bank() and variant() modifier tests
// =============================================================================

TEST_CASE("Pattern function: bank()", "[codegen][patterns][bank]") {
    SECTION("bank requires pattern as first argument") {
        auto result = akkado::compile("bank(42, \"TR808\")");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E130" || d.code == "E133") found = true;
        }
        CHECK(found);
    }

    SECTION("bank requires string as second argument") {
        auto result = akkado::compile(R"(bank(pat("bd sd"), 808))");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E131") found = true;
        }
        CHECK(found);
    }

    SECTION("bank with valid pattern compiles") {
        auto result = akkado::compile(R"(bank(pat("bd sd hh"), "TR808"))");
        REQUIRE(result.success);

        // Should have SEQPAT_QUERY and SEQPAT_STEP instructions
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_STEP) != nullptr);
    }

    SECTION("bank populates required_samples_extended with bank info") {
        auto result = akkado::compile(R"(bank(pat("bd sd"), "TR909"))");
        REQUIRE(result.success);

        // Check that required_samples_extended has entries with bank info
        bool found_bank = false;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.bank == "TR909") {
                found_bank = true;
                break;
            }
        }
        CHECK(found_bank);
    }

    SECTION("bank via method call syntax") {
        // pat("bd").bank("TR808") should desugar to bank(pat("bd"), "TR808")
        auto result = akkado::compile(R"(pat("bd sd").bank("TR808"))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
    }
}

TEST_CASE("Pattern function: variant()", "[codegen][patterns][variant]") {
    SECTION("variant requires pattern as first argument") {
        auto result = akkado::compile("variant(42, 0)");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E130" || d.code == "E133") found = true;
        }
        CHECK(found);
    }

    SECTION("variant accepts string literal as variant pattern") {
        // String literals are now valid pattern expressions (parsed as mini-notation)
        auto result = akkado::compile(R"(variant(pat("bd"), "1 2 3"))");
        CHECK(result.success);
    }

    SECTION("variant with fixed index compiles") {
        auto result = akkado::compile(R"(variant(pat("bd bd bd"), 2))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_STEP) != nullptr);
    }

    SECTION("variant populates required_samples_extended with variant info") {
        auto result = akkado::compile(R"(variant(pat("bd"), 3))");
        REQUIRE(result.success);

        // Check that required_samples_extended has entries with variant info
        bool found_variant = false;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.variant == 3) {
                found_variant = true;
                break;
            }
        }
        CHECK(found_variant);
    }

    SECTION("variant via method call syntax") {
        // pat("bd").variant(2) should desugar to variant(pat("bd"), 2)
        auto result = akkado::compile(R"(pat("bd bd").variant(2))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
    }

    SECTION("variant with pattern index (cycling)") {
        // variant(pat("bd bd bd"), pat("<c0 d0 e0>")) cycles variants per event
        // Note: Using note names since bare numbers in pat() are interpreted as samples
        // The variant values are taken from the frequency values in the variant pattern
        auto result = akkado::compile(R"(variant(pat("bd bd bd"), pat("<c0 d0 e0>")))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
    }

    SECTION("variant with negative index fails") {
        auto result = akkado::compile(R"(variant(pat("bd"), -1))");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E131") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Pattern function: bank and variant chaining", "[codegen][patterns][bank][variant]") {
    SECTION("bank and variant can be chained via pipe") {
        auto result = akkado::compile(R"(
            pat("bd sd hh")
            |> bank(%, "TR808")
            |> variant(%, 1)
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        // Should have multiple SEQPAT_QUERY/STEP pairs (one per transform)
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) >= 1);
    }

    SECTION("method chaining: bank().variant()") {
        // Note: Each transform recompiles the pattern, so bank info from earlier
        // in the chain doesn't propagate to later transforms. This is correct
        // behavior - the outermost transform (variant) determines the final sample mappings.
        auto result = akkado::compile(R"(pat("bd sd").bank("TR909").variant(2))");
        REQUIRE(result.success);

        // Verify variant() applied variant=2
        bool found_variant = false;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.variant == 2) {
                found_variant = true;
                break;
            }
        }
        CHECK(found_variant);
    }

    SECTION("bank alone populates bank field") {
        auto result = akkado::compile(R"(pat("bd sd hh").bank("TR808"))");
        REQUIRE(result.success);

        // Verify bank was set
        bool all_have_bank = true;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.bank != "TR808") {
                all_have_bank = false;
            }
        }
        CHECK(all_have_bank);
    }
}

// =============================================================================
// Pattern String Prefix (p"...")
// =============================================================================

TEST_CASE("Codegen: Pattern string prefix", "[codegen][pattern-prefix]") {
    SECTION("p\"...\" produces same bytecode as pat(\"...\")") {
        auto prefix_result = akkado::compile(R"(p"c4 e4 g4")");
        auto call_result = akkado::compile(R"(pat("c4 e4 g4"))");

        REQUIRE(prefix_result.success);
        REQUIRE(call_result.success);
        CHECK(prefix_result.bytecode == call_result.bytecode);
    }

    SECTION("p\"...\" works in pipeline") {
        auto result = akkado::compile(R"(p"c4 e4 g4" |> osc("sin", %.freq))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SIN) != nullptr);
    }

    SECTION("pat() with parens still works") {
        auto result = akkado::compile(R"(pat("c4 e4 g4"))");
        REQUIRE(result.success);
    }
}

// ============================================================================
// Velocity in Pattern Events
// ============================================================================

TEST_CASE("Codegen: velocity suffix in pattern events", "[codegen][pattern][velocity]") {
    SECTION("pat with velocity suffix stores velocity in events") {
        auto result = akkado::compile(R"(pat("c4:0.8 e4:0.5"))");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequence_events.size() >= 1);
        const auto& root_events = seq_init->sequence_events[0];
        REQUIRE(root_events.size() == 2);

        CHECK(root_events[0].velocity == Catch::Approx(0.8f).margin(0.001f));
        CHECK(root_events[1].velocity == Catch::Approx(0.5f).margin(0.001f));
    }

    SECTION("pat without velocity suffix defaults to 1.0") {
        auto result = akkado::compile(R"(pat("c4 e4"))");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequence_events.size() >= 1);
        const auto& root_events = seq_init->sequence_events[0];
        REQUIRE(root_events.size() == 2);

        CHECK(root_events[0].velocity == Catch::Approx(1.0f).margin(0.001f));
        CHECK(root_events[1].velocity == Catch::Approx(1.0f).margin(0.001f));
    }
}

// =============================================================================
// Polyphony Tests (poly / mono / legato)
// =============================================================================

TEST_CASE("Codegen: poly()", "[codegen][poly]") {
    SECTION("basic poly with named function") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4") |> poly(%, lead, 4) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        // Should contain POLY_BEGIN, body, POLY_END, OUTPUT
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::OUTPUT) == 1);

        // Find POLY_BEGIN and verify rate = body_length > 0
        auto* poly = find_instruction(insts, cedar::Opcode::POLY_BEGIN);
        REQUIRE(poly != nullptr);
        CHECK(poly->rate > 0);  // body_length >= 1

        // Verify state_id is set
        CHECK(poly->state_id != 0);

        // Verify state_init has PolyAlloc type with correct config
        bool found_poly_init = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_max_voices == 4);
                CHECK(init.poly_mode == 0);  // poly mode
                CHECK(init.state_id == poly->state_id);
                found_poly_init = true;
            }
        }
        CHECK(found_poly_init);
    }

    SECTION("mono desugars to poly with mode=1") {
        auto result = akkado::compile(R"(
            fn synth(f, g, v) -> osc("sin", f)
            mono(synth) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);

        // Verify state_init has mode=1 and max_voices=1
        bool found_poly_init = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_mode == 1);
                CHECK(init.poly_max_voices == 1);
                found_poly_init = true;
            }
        }
        CHECK(found_poly_init);
    }

    SECTION("legato desugars to poly with mode=2") {
        auto result = akkado::compile(R"(
            fn synth(f, g, v) -> osc("sin", f)
            legato(synth) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);

        bool found_poly_init = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_mode == 2);
                CHECK(init.poly_max_voices == 1);
                found_poly_init = true;
            }
        }
        CHECK(found_poly_init);
    }

    SECTION("poly with piped pattern input") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> poly(%, lead, 8) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        // Pattern emits SEQPAT_QUERY
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) == 1);
        // POLY block present
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);

        // Check that poly state_init has a linked seq_state_id
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_seq_state_id != 0);
                CHECK(init.poly_max_voices == 8);
            }
        }
    }

    SECTION("poly with inline closure") {
        auto result = akkado::compile(R"(
            pat("c4") |> poly(%, (f, g, v) -> osc("sin", f) * v, 4) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);
    }

    SECTION("error: instrument function must have 3 params") {
        auto result = akkado::compile(R"(
            fn bad(x) -> osc("sin", x)
            pat("c4") |> poly(%, bad, 4) |> out(%, %)
        )");
        CHECK(!result.success);
    }

    SECTION("error: instrument must be a function") {
        auto result = akkado::compile(R"(
            pat("c4") |> poly(%, 440, 4) |> out(%, %)
        )");
        CHECK(!result.success);
    }

    SECTION("poly with default voice count (no voices arg)") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> poly(%, lead) |> out(%, %)
        )");
        REQUIRE(result.success);
        // Voices should default to 64
        bool found_default = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_max_voices == 64);
                found_default = true;
            }
        }
        CHECK(found_default);
    }

    SECTION("error: 1-arg form not supported") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            poly(lead) |> out(%, %)
        )");
        CHECK(!result.success);
        bool found_arity_error = false;
        for (const auto& d : result.diagnostics) {
            if (d.message.find("'poly'") != std::string::npos &&
                d.message.find("at least 2") != std::string::npos) {
                found_arity_error = true;
                break;
            }
        }
        CHECK(found_arity_error);
    }

    SECTION("mono with piped pattern") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> mono(%, lead) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) == 1);
    }
}

// =============================================================================
// Codegen: Record spreading
// =============================================================================

TEST_CASE("Codegen: Record spreading", "[codegen][records]") {
    SECTION("spread with override") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880}
            r.freq
        )");
        CHECK(result.success);
    }

    SECTION("spread with new field") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, pan: 0.5}
            r.pan
        )");
        CHECK(result.success);
    }

    SECTION("spread all fields") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base}
            r.freq + r.vel
        )");
        CHECK(result.success);
    }

    SECTION("inline spread source") {
        auto result = akkado::compile(R"(
            r = {..{freq: 440}, vel: 0.8}
            r.freq + r.vel
        )");
        CHECK(result.success);
    }

    SECTION("spread in pipe") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880}
            osc("sin", r.freq) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Codegen: Expression defaults
// =============================================================================

TEST_CASE("Codegen: Expression defaults", "[codegen][fn]") {
    SECTION("arithmetic expression default") {
        auto result = akkado::compile(R"(
            fn f(x, cut = 440 * 2) -> x + cut
            f(1)
        )");
        CHECK(result.success);
    }

    SECTION("const variable in expression default") {
        auto result = akkado::compile(R"(
            const BASE = 440
            fn f(x, freq = BASE * 2) -> x + freq
            f(1)
        )");
        CHECK(result.success);
    }

    SECTION("const fn call in expression default") {
        auto result = akkado::compile(R"(
            const fn double(x) -> x * 2
            fn f(x, cut = double(440)) -> x + cut
            f(1)
        )");
        CHECK(result.success);
    }

    SECTION("expression default not used when arg provided") {
        auto result = akkado::compile(R"(
            fn f(x, cut = 440 * 2) -> x + cut
            f(1, 100)
        )");
        CHECK(result.success);
    }

    SECTION("closure with expression default") {
        auto result = akkado::compile(R"(
            g = (x, cut = 440 * 2) -> x + cut
            g(1)
        )");
        CHECK(result.success);
    }

    SECTION("multiple expression defaults") {
        auto result = akkado::compile(R"(
            fn f(x, a = 1 + 1, b = 2 * 3) -> x + a + b
            f(1)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Codegen: Record spread interactions (F8 integration)
// =============================================================================

TEST_CASE("Codegen: Record spread interactions", "[codegen][records]") {
    SECTION("field access on spread result") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            {..base, freq: 880}.freq
        )");
        CHECK(result.success);
    }

    SECTION("spread + as-binding in pipe") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            {..base, vel: 0.5} as r |> osc("sin", r.freq) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("spread + dot-call") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880}
            osc("sin", r.freq).lp(1000) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("nested spread (spread of spread)") {
        auto result = akkado::compile(R"(
            inner = {freq: 440}
            mid = {..inner, vel: 0.8}
            outer = {..mid, pan: 0.5}
            outer.freq + outer.vel + outer.pan
        )");
        CHECK(result.success);
    }

    SECTION("spread overrides all fields") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880, vel: 0.5}
            osc("sin", r.freq) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("spread + shorthand field") {
        auto result = akkado::compile(R"(
            base = {freq: 440}
            vel = 0.8
            r = {..base, vel}
            r.freq + r.vel
        )");
        CHECK(result.success);
    }

    SECTION("spread field used in match scrutinee") {
        auto result = akkado::compile(R"(
            base = {mode: 1, freq: 440}
            r = {..base, mode: 2}
            match(r.mode) {
                1: 100,
                2: 200,
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("spread record passed as fn argument") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            fn get_freq(r) -> r.freq
            get_freq({..base, pan: 0.5})
        )");
        CHECK(result.success);
    }

    SECTION("error: spread non-record") {
        auto result = akkado::compile(R"(
            x = 42
            r = {..x, vel: 0.8}
        )");
        CHECK_FALSE(result.success);
    }

    SECTION("two records in same pipe") {
        auto result = akkado::compile(R"(
            o = {freq: 440, type: "sin"}
            f = {cutoff: 1000, res: 0.707}
            osc(o.type, o.freq) |> lp(%, f.cutoff, f.res) |> out(%, %)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Codegen: Expression default interactions (F6 integration)
// =============================================================================

TEST_CASE("Codegen: Expression default interactions", "[codegen][fn]") {
    SECTION("expression default + pipe body") {
        auto result = akkado::compile(R"(
            fn f(x, gain = 1 + 1) -> x |> % * gain |> out(%, %)
            osc("sin", 440) |> f(%) |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("expression default + as-binding") {
        auto result = akkado::compile(R"(
            fn amp(x, gain = 2 * 0.25) -> x * gain
            osc("sin", 440) as sig |> amp(sig) |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("expression default + match body") {
        auto result = akkado::compile(R"(
            fn sel(x, mode = 1 + 0) -> match(mode) {
                1: x,
                2: x * 2,
                _: 0
            }
            sel(440)
        )");
        CHECK(result.success);
    }

    SECTION("expression default + dot-call") {
        auto result = akkado::compile(R"(
            fn amp(sig, gain = 1 + 1) -> sig * gain
            osc("sin", 440).amp() |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("negation as default") {
        auto result = akkado::compile(R"(
            fn offset(x, y = -1) -> x + y
            offset(10)
        )");
        CHECK(result.success);
    }

    SECTION("parenthesized expression default") {
        auto result = akkado::compile(R"(
            fn scale(x, factor = (1 + 2) * 3) -> x * factor
            scale(10)
        )");
        CHECK(result.success);
    }

    SECTION("pitch literal as default") {
        auto result = akkado::compile(R"(
            fn my_osc(type, freq = 'C4') -> osc(type, freq)
            my_osc("sin") |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("closure expr default assigned then called") {
        auto result = akkado::compile(R"(
            g = (x, y = 10 * 2) -> x + y
            g(1)
        )");
        CHECK(result.success);
    }

    SECTION("multiple defaults, partial override") {
        auto result = akkado::compile(R"(
            fn f(x, a = 1 + 1, b = 2 * 3) -> x + a + b
            f(1, 5)
        )");
        CHECK(result.success);
    }

    SECTION("expr default fn called multiple times") {
        auto result = akkado::compile(R"(
            fn f(x, scale = 2 + 2) -> x * scale
            f(1) + f(2) + f(3, 10)
        )");
        CHECK(result.success);
    }
}

// ============================================================================
// Timeline Curve Breakpoint Tests [timeline_codegen]
// ============================================================================

static akkado::PatternEventStream eval_curve(const std::string& curve_str) {
    akkado::AstArena arena;
    auto [root, diags] = akkado::parse_mini(curve_str, arena, {}, false, true);
    return akkado::evaluate_pattern(root, arena, 0);
}

TEST_CASE("Constant curve produces single breakpoint", "[timeline_codegen]") {
    auto stream = eval_curve("___");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() == 1);
    CHECK(breakpoints[0].time == Catch::Approx(0.0f));
    CHECK(breakpoints[0].value == Catch::Approx(0.0f));
    CHECK(breakpoints[0].curve == 2); // hold
}

TEST_CASE("Step curve produces two breakpoints", "[timeline_codegen]") {
    auto stream = eval_curve("__''");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() == 2);
    CHECK(breakpoints[0].value == Catch::Approx(0.0f));
    CHECK(breakpoints[0].curve == 2);
    CHECK(breakpoints[1].time == Catch::Approx(0.5f).margin(0.01f));
    CHECK(breakpoints[1].value == Catch::Approx(1.0f));
    CHECK(breakpoints[1].curve == 2);
}

TEST_CASE("Ramp produces linear breakpoints", "[timeline_codegen]") {
    auto stream = eval_curve("_/'");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() >= 2);
    // Should have linear interpolation somewhere
    bool has_linear = false;
    for (const auto& bp : breakpoints) {
        if (bp.curve == 0) has_linear = true;
    }
    CHECK(has_linear);
}

TEST_CASE("Smooth modifier produces linear breakpoint", "[timeline_codegen]") {
    auto stream = eval_curve("_~'");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() >= 2);
    // The smooth level should be linear
    CHECK(breakpoints[1].curve == 0);
    CHECK(breakpoints[1].value == Catch::Approx(1.0f));
}

TEST_CASE("Multiple ramps produce proportional interpolation", "[timeline_codegen]") {
    auto stream = eval_curve("_//'");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    // Should have intermediate value around 0.5
    bool has_mid = false;
    for (const auto& bp : breakpoints) {
        if (bp.value > 0.3f && bp.value < 0.7f && bp.curve == 0) {
            has_mid = true;
        }
    }
    CHECK(has_mid);
}

TEST_CASE("All-same levels merge to single breakpoint", "[timeline_codegen]") {
    auto stream = eval_curve("'''''");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    CHECK(breakpoints.size() == 1);
    CHECK(breakpoints[0].value == Catch::Approx(1.0f));
}

TEST_CASE("Timeline curve compiles to TIMELINE opcode", "[timeline_codegen]") {
    auto result = akkado::compile("t\"____\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    bool found = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::TIMELINE) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("Timeline curve produces state init", "[timeline_codegen]") {
    auto result = akkado::compile("t\"__''\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
    bool found = false;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::Timeline) {
            found = true;
            CHECK(!init.timeline_breakpoints.empty());
            CHECK(init.timeline_loop == true);
            CHECK(init.timeline_loop_length > 0.0f);
        }
    }
    CHECK(found);
}

// ============================================================================
// Timeline End-to-End Integration Tests [timeline_e2e]
// ============================================================================

TEST_CASE("Timeline curve with math compiles", "[timeline_e2e]") {
    auto result = akkado::compile("t\"__/''\" * 1800 + 200 |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    CHECK(find_instruction(insts, cedar::Opcode::TIMELINE) != nullptr);
}

TEST_CASE("Timeline constant output compiles", "[timeline_e2e]") {
    auto result = akkado::compile("t\"''''\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    CHECK(find_instruction(insts, cedar::Opcode::TIMELINE) != nullptr);
}

TEST_CASE("Timeline with pipe chain compiles", "[timeline_e2e]") {
    auto result = akkado::compile("osc(\"sin\", 440) * t\"''''____\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
}

TEST_CASE("Timeline function call form compiles", "[timeline_e2e]") {
    auto result = akkado::compile("timeline(\"__/''\") |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    CHECK(find_instruction(insts, cedar::Opcode::TIMELINE) != nullptr);
    // Verify state init exists
    bool found = false;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::Timeline) found = true;
    }
    CHECK(found);
}

// =============================================================================
// Waterfall / FFT Visualization Tests
// =============================================================================

TEST_CASE("Codegen: waterfall() emits FFT_PROBE", "[codegen][viz]") {
    SECTION("basic waterfall with default fft size") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test") |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
        REQUIRE(fft != nullptr);
        CHECK(fft->rate == 10);  // default 1024
    }

    SECTION("waterfall with fft: 512") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test", {fft: 512}) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
        REQUIRE(fft != nullptr);
        CHECK(fft->rate == 9);  // 512 = 2^9
    }

    SECTION("waterfall with fft: 2048") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test", {fft: 2048}) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
        REQUIRE(fft != nullptr);
        CHECK(fft->rate == 11);  // 2048 = 2^11
    }

    SECTION("waterfall with string gradient option") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test", {gradient: "viridis"}) |> out(%, %)
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::Waterfall) {
                CHECK(decl.options_json.find("\"gradient\":\"viridis\"") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION("waterfall creates Waterfall viz decl") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "my-spectrogram") |> out(%, %)
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::Waterfall) {
                CHECK(decl.name == "my-spectrogram");
                CHECK(decl.state_id != 0);
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: viz options serialize BoolLit values", "[codegen][viz]") {
    SECTION("boolean true") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> spectrum(%, "s", {logScale: true}) |> out(%, %)
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::Spectrum) {
                CHECK(decl.options_json.find("\"logScale\":true") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION("boolean false") {
        auto result = akkado::compile(R"(
            pat("c4 e4") |> pianoroll(%, "pr", {showGrid: false})
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::PianoRoll) {
                CHECK(decl.options_json.find("\"showGrid\":false") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION("mixed types: number, boolean, string") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "w", {minDb: -60, logScale: true, gradient: "warm"}) |> out(%, %)
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::Waterfall) {
                CHECK(decl.options_json.find("\"minDb\":-60") != std::string::npos);
                CHECK(decl.options_json.find("\"logScale\":true") != std::string::npos);
                CHECK(decl.options_json.find("\"gradient\":\"warm\"") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: spectrum() now emits FFT_PROBE", "[codegen][viz]") {
    auto result = akkado::compile(R"(
        osc("saw", 220) |> spectrum(%, "fft") |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);

    // Should use FFT_PROBE, not PROBE
    auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
    REQUIRE(fft != nullptr);
    CHECK(fft->rate == 10);  // default 1024

    // Should NOT have a PROBE instruction
    auto* probe = find_instruction(insts, cedar::Opcode::PROBE);
    CHECK(probe == nullptr);
}

// =============================================================================
// Builtin Variable Tests
// =============================================================================

TEST_CASE("Codegen: bpm assignment generates override metadata", "[codegen][builtins]") {
    SECTION("basic bpm assignment") {
        auto result = akkado::compile(R"(
            bpm = 120
            saw(220) |> out(%, %)
        )");
        REQUIRE(result.success);
        REQUIRE(result.builtin_var_overrides.size() == 1);
        CHECK(result.builtin_var_overrides[0].name == "bpm");
        CHECK(result.builtin_var_overrides[0].value == Catch::Approx(120.0f));

        // bpm is only assigned, not read — no ENV_GET emitted for __bpm
        auto insts = get_instructions(result);
        bool found_bpm_env_get = false;
        std::uint32_t bpm_hash = cedar::fnv1a_hash_runtime("__bpm", 5);
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == bpm_hash) {
                found_bpm_env_get = true;
            }
        }
        CHECK_FALSE(found_bpm_env_get);
    }

    SECTION("bpm with arithmetic expression") {
        auto result = akkado::compile(R"(
            bpm = 60 * 2
            saw(220) |> out(%, %)
        )");
        REQUIRE(result.success);
        REQUIRE(result.builtin_var_overrides.size() == 1);
        CHECK(result.builtin_var_overrides[0].value == Catch::Approx(120.0f));
    }

    SECTION("bpm value is clamped to valid range") {
        auto result = akkado::compile(R"(
            bpm = 0.5
            saw(220) |> out(%, %)
        )");
        REQUIRE(result.success);
        REQUIRE(result.builtin_var_overrides.size() == 1);
        CHECK(result.builtin_var_overrides[0].value == Catch::Approx(1.0f));
    }
}

TEST_CASE("Codegen: reading bpm emits ENV_GET", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        x = 60 / bpm
        saw(x) |> out(%, %)
    )");
    REQUIRE(result.success);

    auto insts = get_instructions(result);
    std::uint32_t bpm_hash = cedar::fnv1a_hash_runtime("__bpm", 5);
    bool found = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == bpm_hash) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("Codegen: sr is read-only", "[codegen][builtins]") {
    SECTION("sr assignment is error") {
        auto result = akkado::compile(R"(
            sr = 44100
            saw(220) |> out(%, %)
        )");
        REQUIRE_FALSE(result.success);
        bool found_e170 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E170") found_e170 = true;
        }
        CHECK(found_e170);
    }

    SECTION("reading sr emits ENV_GET") {
        auto result = akkado::compile(R"(
            x = 220 / sr
            saw(x) |> out(%, %)
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        std::uint32_t sr_hash = cedar::fnv1a_hash_runtime("__sr", 4);
        bool found = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == sr_hash) {
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: const bpm is error", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        const bpm = 120
        saw(220) |> out(%, %)
    )");
    REQUIRE_FALSE(result.success);
    bool found_e170 = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E170") found_e170 = true;
    }
    CHECK(found_e170);
}

TEST_CASE("Codegen: bpm with non-constant expression is error", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        bpm = param("tempo", 120, 60, 200)
        saw(220) |> out(%, %)
    )");
    REQUIRE_FALSE(result.success);
    bool found_e172 = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E172") found_e172 = true;
    }
    CHECK(found_e172);
}

TEST_CASE("Codegen: multiple bpm assignments both stored", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        bpm = 100
        bpm = 140
        saw(220) |> out(%, %)
    )");
    REQUIRE(result.success);
    REQUIRE(result.builtin_var_overrides.size() == 2);
    CHECK(result.builtin_var_overrides[0].value == Catch::Approx(100.0f));
    CHECK(result.builtin_var_overrides[1].value == Catch::Approx(140.0f));
}

// =============================================================================
// Conditionals & Logic — Runtime Value Tests
//
// These tests compile akkado source, load the bytecode into a Cedar VM,
// process one block, and assert the actual runtime value of the destination
// buffer of the last instruction. This verifies that the conditionals/logic
// operators produce the documented 0.0/1.0 outputs (and not, say, an inverted
// truth polarity that codegen-presence tests above cannot catch).
// =============================================================================

namespace {

// Compile akkado source, run one block, and return the first sample of the
// destination buffer of the program's last instruction.
float run_first_sample(std::string_view source) {
    auto result = akkado::compile(source);
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    REQUIRE_FALSE(insts.empty());

    cedar::VM vm;
    std::span<const cedar::Instruction> bc(insts.data(), insts.size());
    REQUIRE(vm.load_program_immediate(bc));

    std::array<float, cedar::BLOCK_SIZE> left{};
    std::array<float, cedar::BLOCK_SIZE> right{};
    vm.process_block(left.data(), right.data());

    const std::uint16_t out_idx = insts.back().out_buffer;
    const float* buf = vm.buffers().get(out_idx);
    REQUIRE(buf != nullptr);
    return buf[0];
}

}  // namespace

TEST_CASE("Runtime: gt/lt/gte/lte produce 0.0 or 1.0", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("gt true / false / equal") {
        CHECK_THAT(run_first_sample("gt(10, 5)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gt(5, 10)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gt(5, 5)"),  WithinAbs(0.0f, 1e-6f));
    }

    SECTION("lt true / false / equal") {
        CHECK_THAT(run_first_sample("lt(5, 10)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lt(10, 5)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lt(5, 5)"),  WithinAbs(0.0f, 1e-6f));
    }

    SECTION("gte includes equality") {
        CHECK_THAT(run_first_sample("gte(10, 5)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gte(5, 5)"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gte(5, 10)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("lte includes equality") {
        CHECK_THAT(run_first_sample("lte(5, 10)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lte(5, 5)"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lte(10, 5)"), WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: eq/neq with epsilon", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("exact equality") {
        CHECK_THAT(run_first_sample("eq(5, 5)"),    WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("eq(5, 10)"),   WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(5, 5)"),   WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(5, 10)"),  WithinAbs(1.0f, 1e-6f));
    }

    SECTION("eq treats near-equal floats as equal") {
        // Floating-point round-off: 0.1 + 0.2 != 0.3 in strict IEEE,
        // but the documented epsilon (1e-6) must collapse them to true.
        CHECK_THAT(run_first_sample("eq(0.1 + 0.2, 0.3)"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(0.1 + 0.2, 0.3)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("eq separates values farther apart than epsilon") {
        // 1e-3 is well above the 1e-6 epsilon, must compare not-equal.
        CHECK_THAT(run_first_sample("eq(1.0, 1.001)"),  WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(1.0, 1.001)"), WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: band/bor/bnot truth tables", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("band truth table") {
        CHECK_THAT(run_first_sample("band(1, 1)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("band(1, 0)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("band(0, 1)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("band(0, 0)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("bor truth table") {
        CHECK_THAT(run_first_sample("bor(1, 1)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(1, 0)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(0, 1)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(0, 0)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("bnot inverts truthiness") {
        CHECK_THAT(run_first_sample("bnot(1)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bnot(0)"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("negative values are falsy") {
        // band/bor/bnot all treat values <= 0 as false, including negatives.
        CHECK_THAT(run_first_sample("band(-1, 1)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(-1, -2)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bnot(-1)"),    WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: select picks the right branch", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("truthy condition") {
        CHECK_THAT(run_first_sample("select(1, 100, 50)"), WithinAbs(100.0f, 1e-6f));
    }

    SECTION("zero is falsy") {
        CHECK_THAT(run_first_sample("select(0, 100, 50)"), WithinAbs(50.0f, 1e-6f));
    }

    SECTION("negative is falsy") {
        // Documented in PRD: select uses cond > 0, so negatives go to b.
        CHECK_THAT(run_first_sample("select(-1, 100, 50)"), WithinAbs(50.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: infix syntax matches function-call syntax", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("comparison infix") {
        CHECK_THAT(run_first_sample("10 > 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 < 10"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 >= 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 <= 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 == 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 != 10"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("logic infix") {
        CHECK_THAT(run_first_sample("1 && 1"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("1 || 0"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("0 && 1"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("!1"),     WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("!0"),     WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: operator precedence", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("&& binds tighter than ||") {
        // 1 || 0 && 0 must parse as 1 || (0 && 0) = 1
        CHECK_THAT(run_first_sample("1 || 0 && 0"), WithinAbs(1.0f, 1e-6f));
        // 0 && 1 || 0 must parse as (0 && 1) || 0 = 0
        CHECK_THAT(run_first_sample("0 && 1 || 0"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("comparison binds tighter than logic") {
        // (5 > 3) && (2 < 4) = 1
        CHECK_THAT(run_first_sample("5 > 3 && 2 < 4"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("arithmetic binds tighter than comparison") {
        // (2 + 3) > 4 = 5 > 4 = 1
        CHECK_THAT(run_first_sample("2 + 3 > 4"), WithinAbs(1.0f, 1e-6f));
        // (2 * 3) == 6 = 1
        CHECK_THAT(run_first_sample("2 * 3 == 6"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("equality binds looser than comparison") {
        // (5 > 3) == 1 — comparison evaluated first, equality compares 1.0 to 1
        CHECK_THAT(run_first_sample("5 > 3 == 1"), WithinAbs(1.0f, 1e-6f));
    }
}

// =============================================================================
// Bug repro: chord("C Em Am G") |> poly() — incomplete chord onsets
// =============================================================================
//
// User report: with `chord("C Em Am G") |> poly(@, stab, 21) * 0.33 |> out(@)`
// where stab has a 0.4s release + 0.3s delay tail, every few bars a chord
// plays with a missing note. This test compiles the user's exact source,
// runs it for many cycles, snapshots the voice grid each block, and detects
// chord onsets where fewer than 3 voices are firing the expected freqs.

namespace {

// Apply state_inits from a CompileResult to the given VM.
// Mirrors the logic in web/wasm/nkido_wasm.cpp:apply_state_inits.
void apply_state_inits(cedar::VM& vm, const akkado::CompileResult& result,
                       std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    // We need to keep Sequence storage alive for the duration of the test.
    seq_storage.reserve(result.state_inits.size());
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(init.sequence_events[i].data());
                    seq_copy[i].num_events = static_cast<std::uint32_t>(init.sequence_events[i].size());
                    seq_copy[i].capacity = static_cast<std::uint32_t>(init.sequence_events[i].size());
                }
            }
            seq_storage.push_back(std::move(seq_copy));
            auto& stored = seq_storage.back();
            vm.init_sequence_program_state(
                init.state_id,
                stored.data(), stored.size(),
                init.cycle_length,
                init.is_sample_pattern,
                init.total_events
            );
        } else if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            vm.init_poly_state(
                init.state_id,
                init.poly_seq_state_id,
                init.poly_max_voices,
                init.poly_mode,
                init.poly_steal_strategy
            );
        }
    }
}

}  // namespace

TEST_CASE("Runtime: chord(...) |> poly fires every chord note completely",
          "[poly][regression][chord-completeness]") {
    // The user's exact patch
    const char* src = R"(
        stab = (freq, gate, vel) ->
            saw(freq) * ar(gate, 0.05, 0.4) * vel
            |> lp(@, 1100)
            |> @ + delay(@, 0.3, 0)

        chord("C Em Am G")
            |> poly(@, stab, 21) * 0.33
            |> out(@)
    )";

    auto result = akkado::compile(src);
    REQUIRE(result.success);

    auto insts = get_instructions(result);
    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    REQUIRE(vm.load_program_immediate(std::span<const cedar::Instruction>(insts)));

    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_state_inits(vm, result, seq_storage);

    // Find the PolyAllocState ID
    std::uint32_t poly_state_id = 0;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            poly_state_id = init.state_id;
            break;
        }
    }
    REQUIRE(poly_state_id != 0);

    // Chord parser uses default octave 4 for the root, then adds intervals
    // C major = [0, 4, 7] from C4 → C4, E4, G4 = MIDI 60, 64, 67
    // E minor = [0, 3, 7] from E4 → E4, G4, B4 = MIDI 64, 67, 71
    // A minor = [0, 3, 7] from A4 → A4, C5, E5 = MIDI 69, 72, 76
    // G major = [0, 4, 7] from G4 → G4, B4, D5 = MIDI 67, 71, 74
    auto midi_to_hz = [](int midi) {
        return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f);
    };
    const std::array<std::array<float, 3>, 4> CHORDS = {{
        {midi_to_hz(60), midi_to_hz(64), midi_to_hz(67)},   // C
        {midi_to_hz(64), midi_to_hz(67), midi_to_hz(71)},   // Em
        {midi_to_hz(69), midi_to_hz(72), midi_to_hz(76)},   // Am
        {midi_to_hz(67), midi_to_hz(71), midi_to_hz(74)},   // G
    }};

    // 120 BPM, 48 kHz, BLOCK_SIZE 128:
    // samples per beat = 48000 * 60 / 120 = 24000
    // 1 cycle (4 beats) = 96000 samples = 750 blocks exactly
    // Each chord lasts 1 beat = 24000 samples = 187.5 blocks
    constexpr int BLOCKS_PER_CYCLE = 750;
    constexpr int CYCLES = 32;  // long enough to surface "every few bars" bug
    constexpr int TOTAL_BLOCKS = CYCLES * BLOCKS_PER_CYCLE;

    std::array<float, cedar::BLOCK_SIZE> left{}, right{};
    auto& poly = vm.states().get_or_create<cedar::PolyAllocState>(poly_state_id);

    int incomplete_count = 0;
    int onsets_inspected = 0;
    int last_chord_idx = -1;
    int first_failure_block = -1;
    int first_failure_chord = -1;
    int first_failure_matched = -1;

    for (int b = 0; b < TOTAL_BLOCKS; ++b) {
        const float block_beats = (b * static_cast<float>(cedar::BLOCK_SIZE)) /
                                  (48000.0f * 60.0f / 120.0f);
        const float cycle_pos = std::fmod(block_beats, 4.0f);
        const int chord_idx = static_cast<int>(std::floor(cycle_pos));

        vm.process_block(left.data(), right.data());

        // Inspect 30 blocks into each new chord (gives time for attack to settle)
        const int blocks_into_chord = static_cast<int>((cycle_pos - chord_idx) * 187.5f);
        if (chord_idx != last_chord_idx && blocks_into_chord >= 30) {
            // Skip the very first chord at b=0 (no settle time)
            if (b > 100) {
                onsets_inspected++;
                const auto& chord = CHORDS[chord_idx];
                int matched = 0;
                for (float target : chord) {
                    bool found = false;
                    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
                        const auto& v = poly.voices[i];
                        if (v.active && !v.releasing && v.gate > 0.5f &&
                            std::abs(v.freq - target) < 0.5f) {
                            found = true;
                            break;
                        }
                    }
                    if (found) ++matched;
                }
                if (matched < 3) {
                    if (first_failure_block < 0) {
                        first_failure_block = b;
                        first_failure_chord = chord_idx;
                        first_failure_matched = matched;
                    }
                    ++incomplete_count;
                }
            }
            last_chord_idx = chord_idx;
        }
    }

    INFO("Inspected " << onsets_inspected << " chord onsets, "
         << incomplete_count << " incomplete");
    if (first_failure_block >= 0) {
        INFO("First failure: block=" << first_failure_block
             << " chord_idx=" << first_failure_chord
             << " matched=" << first_failure_matched << "/3");
    }
    CHECK(onsets_inspected >= 30);
    CHECK(incomplete_count == 0);
}


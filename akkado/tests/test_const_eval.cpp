#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <cmath>
#include <vector>

// Helper to decode float from PUSH_CONST instruction
static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

static std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

static size_t count_instructions(const std::vector<cedar::Instruction>& insts,
                                  cedar::Opcode op) {
    size_t count = 0;
    for (const auto& inst : insts) {
        if (inst.opcode == op) ++count;
    }
    return count;
}

// =============================================================================
// const variable declarations
// =============================================================================

TEST_CASE("Const: scalar variable", "[const]") {
    auto result = akkado::compile("const x = 42");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    REQUIRE(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
    CHECK(decode_const_float(insts[0]) == 42.0f);
}

TEST_CASE("Const: arithmetic expression", "[const]") {
    auto result = akkado::compile("const y = 2 * 220 + 5");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Should emit a single PUSH_CONST with value 445
    REQUIRE(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
    CHECK(decode_const_float(insts[0]) == 445.0f);
}

TEST_CASE("Const: use const variable in expression", "[const]") {
    auto result = akkado::compile(R"(
        const base = 440
        osc("sin", base)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // base should be a PUSH_CONST(440)
    CHECK(decode_const_float(insts[0]) == 440.0f);
}

TEST_CASE("Const: nested const references", "[const]") {
    auto result = akkado::compile(R"(
        const a = 100
        const b = a * 2
        const c = a + b
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // a = 100, b = 200, c = 300
    CHECK(decode_const_float(insts[0]) == 100.0f);
    CHECK(decode_const_float(insts[1]) == 200.0f);
    CHECK(decode_const_float(insts[2]) == 300.0f);
}

// =============================================================================
// const fn declarations
// =============================================================================

TEST_CASE("Const fn: simple function", "[const]") {
    auto result = akkado::compile(R"(
        const fn double_it(x) -> x * 2
        osc("sin", double_it(220))
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // double_it(220) should be evaluated at compile time to 440
    // Find the PUSH_CONST instructions
    bool found_440 = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            if (decode_const_float(inst) == 440.0f) {
                found_440 = true;
            }
        }
    }
    CHECK(found_440);
}

TEST_CASE("Const fn: mtof conversion", "[const]") {
    auto result = akkado::compile(R"(
        const fn mtof(n) -> 440 * pow(2, (n - 69) / 12)
        osc("sin", mtof(69))
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // mtof(69) = 440 * pow(2, 0) = 440
    bool found_440 = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            if (decode_const_float(inst) == Catch::Approx(440.0f).margin(0.01f)) {
                found_440 = true;
            }
        }
    }
    CHECK(found_440);
}

TEST_CASE("Const fn: with const variable argument", "[const]") {
    auto result = akkado::compile(R"(
        const a = 60
        const fn mtof(n) -> 440 * pow(2, (n - 69) / 12)
        const freq = mtof(a)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // mtof(60) ≈ 261.626
    bool found_freq = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            float val = decode_const_float(inst);
            if (std::abs(val - 261.626f) < 0.1f) {
                found_freq = true;
            }
        }
    }
    CHECK(found_freq);
}

TEST_CASE("Const fn: with default parameter", "[const]") {
    auto result = akkado::compile(R"(
        const fn scale(x, factor = 2) -> x * factor
        osc("sin", scale(220))
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // scale(220) = 220 * 2 = 440
    bool found_440 = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            if (decode_const_float(inst) == 440.0f) {
                found_440 = true;
            }
        }
    }
    CHECK(found_440);
}

// =============================================================================
// const with array generation functions
// =============================================================================

TEST_CASE("Const: linspace accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const count = 4
        linspace(0, 1, count)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Should produce 4 PUSH_CONST values: 0.0, 0.333, 0.667, 1.0
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 5);  // count + linspace values
}

TEST_CASE("Const: range accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const start = 0
        const end = 4
        range(start, end)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // range(0, 4) = [0, 1, 2, 3] -> 4 values plus the const definitions
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 6);  // 2 consts + 4 range values
}

TEST_CASE("Const: harmonics accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const fund = 100
        const count = 3
        harmonics(fund, count)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // harmonics(100, 3) = [100, 200, 300]
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 5);
}

TEST_CASE("Const: random accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const count = 3
        random(count)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // random(3) = 3 random values + 1 const
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 4);
}

// =============================================================================
// Purity errors
// =============================================================================

TEST_CASE("Const fn: rejects non-pure operations", "[const]") {
    auto result = akkado::compile(R"(
        const fn bad(x) -> osc("sin", x)
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const fn: rejects non-const function calls", "[const]") {
    auto result = akkado::compile(R"(
        fn regular(x) -> x * 2
        const fn uses_regular(x) -> regular(x)
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const: rejects non-pure rhs", "[const]") {
    auto result = akkado::compile(R"(
        const x = param("freq", 440)
    )");
    CHECK_FALSE(result.success);
}

// =============================================================================
// Const fn: math builtins
// =============================================================================

TEST_CASE("Const fn: uses math builtins", "[const]") {
    auto result = akkado::compile(R"(
        const fn db_to_amp(db) -> pow(10, db / 20)
        osc("sin", 440) |> % * db_to_amp(-6)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // db_to_amp(-6) ≈ 0.501
    bool found = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            float val = decode_const_float(inst);
            if (std::abs(val - 0.501f) < 0.01f) {
                found = true;
            }
        }
    }
    CHECK(found);
}

TEST_CASE("Const: division by zero error", "[const]") {
    auto result = akkado::compile("const x = 1 / 0");
    CHECK_FALSE(result.success);
}

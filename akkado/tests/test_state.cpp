// Userspace state cells (state/get/set) — Phase 3 of
// prd-userspace-state-and-edge-primitives.md.
//
// Verifies: parse, codegen, and runtime behavior of state(init), .get(),
// .set(v); reserved-name enforcement at parser level; that distinct call
// sites get independent slots; type errors on non-cell args.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <cstring>
#include <vector>

static std::vector<cedar::Instruction> get_instructions(
    const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> insts;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    insts.resize(count);
    std::memcpy(insts.data(), result.bytecode.data(), result.bytecode.size());
    return insts;
}

static size_t count_op(const std::vector<cedar::Instruction>& insts,
                       cedar::Opcode op, std::uint8_t rate = 0xFF) {
    size_t n = 0;
    for (const auto& i : insts) {
        if (i.opcode != op) continue;
        if (rate != 0xFF && i.rate != rate) continue;
        ++n;
    }
    return n;
}

static bool diagnostic_contains(const akkado::CompileResult& r, const std::string& s) {
    for (const auto& d : r.diagnostics) {
        if (d.message.find(s) != std::string::npos) return true;
    }
    return false;
}

TEST_CASE("State: state(init) emits STATE_OP rate=0", "[state]") {
    auto r = akkado::compile(
        "s = state(0)\n"
        "out(s.get(), s.get())\n"
    );
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 0) == 1);  // exactly one init
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 1) >= 1);  // at least one load
}

TEST_CASE("State: distinct call sites get distinct slots", "[state][slots]") {
    auto r = akkado::compile(
        "a = state(0)\n"
        "b = state(0)\n"
        "out(a.get(), b.get())\n"
    );
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    // Find the two STATE_OP rate=0 (init) instructions and verify their
    // state_ids differ — they came from distinct AST positions.
    std::vector<std::uint32_t> init_ids;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::STATE_OP && i.rate == 0) {
            init_ids.push_back(i.state_id);
        }
    }
    REQUIRE(init_ids.size() == 2);
    CHECK(init_ids[0] != init_ids[1]);
}

TEST_CASE("State: get/set chain stores and reads correctly at runtime", "[state][runtime]") {
    // Cell starts at 0, set to 42, then get — should output 42.
    // s.set(42) is a bare expression statement (its return value is unused).
    auto r = akkado::compile(
        "s = state(0)\n"
        "s.set(42)\n"
        "out(s.get(), s.get())\n"
    );
    REQUIRE(r.success);

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // First block: set fires once, then get reads. set writes the LAST sample
    // of its input buffer (which is broadcast 42.0) into the slot, then get
    // broadcasts that value to its output.
    CHECK(L[cedar::BLOCK_SIZE - 1] == 42.0f);
    CHECK(R[cedar::BLOCK_SIZE - 1] == 42.0f);
}

TEST_CASE("State: cell value persists across blocks", "[state][persistence]") {
    auto r = akkado::compile(
        "s = state(7)\n"
        "out(s.get(), s.get())\n"
    );
    REQUIRE(r.success);

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int block = 0; block < 5; ++block) {
        vm.process_block(L.data(), R.data());
        // Init runs once; subsequent blocks see preserved value 7.
        CHECK(L[0] == 7.0f);
        CHECK(L[cedar::BLOCK_SIZE - 1] == 7.0f);
    }
}

TEST_CASE("State: get on non-cell argument is an error", "[state][types]") {
    auto r = akkado::compile(
        "x = 5\n"
        "out(get(x), get(x))\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "state cell"));
}

TEST_CASE("State: set on non-cell first argument is an error", "[state][types]") {
    auto r = akkado::compile(
        "x = 5\n"
        "out(set(x, 1), set(x, 1))\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "state cell"));
}

TEST_CASE("State: 'state' is a reserved identifier", "[state][reserved]") {
    auto r = akkado::compile("state = 5\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

TEST_CASE("State: 'get' is a reserved identifier", "[state][reserved]") {
    auto r = akkado::compile("get = 5\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

TEST_CASE("State: 'set' is a reserved identifier", "[state][reserved]") {
    auto r = akkado::compile("set = 5\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

TEST_CASE("State: defining a function named 'state' is a reserved-name error",
          "[state][reserved]") {
    auto r = akkado::compile("fn state(x) -> x + 1\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

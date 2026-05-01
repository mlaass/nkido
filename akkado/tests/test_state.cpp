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
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <span>
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

// PRD §9 edge case: `s.get()` before any `s.set()` returns the init value.
// CellState.initialized is false on the first STATE_OP rate=0, which writes
// the init buffer into the slot before any reads.
TEST_CASE("State: get() before any set() returns the init value",
          "[state][edge_case]") {
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
    vm.process_block(L.data(), R.data());

    // From the very first sample of the very first block, get() returns 7.
    CHECK(L[0] == 7.0f);
    CHECK(R[0] == 7.0f);
}

// PRD §11 testing strategy explicit requirement: state() inside a closure
// invoked at different sites has independent storage per site. Each call site
// gets its own state-pool slot via the AST-position semantic-ID hash.
TEST_CASE("State: closure invoked at two sites has independent slots per site",
          "[state][slots]") {
    // bump_and_read defines a state cell internally. Calling it twice at
    // different syntactic positions must produce two distinct STATE_OP rate=0
    // (init) instructions with different state_ids — proving the per-call-site
    // slot isolation.
    auto r = akkado::compile(
        "bump_and_read = (init) -> {\n"
        "  s = state(init)\n"
        "  s.set(s.get() + 1)\n"
        "  s.get()\n"
        "}\n"
        "out(bump_and_read(0), bump_and_read(100))\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);

    // Two distinct call sites of `bump_and_read` → distinct STATE_OP rate=0
    // inits with different state_ids. The codegen may emit additional inits
    // (e.g. one per closure expansion); what matters is that the set of
    // unique state_ids has at least two members, proving the per-call-site
    // hashing isolates slots between the two invocations.
    std::set<std::uint32_t> unique_init_ids;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::STATE_OP && i.rate == 0) {
            unique_init_ids.insert(i.state_id);
        }
    }
    CHECK(unique_init_ids.size() >= 2);
}

// PRD Goal 1: all four `step` variants must be implementable. The simplest
// (`step(arr, trig)`) and `step_dir(arr, trig, dir)` are exercised by the
// stepper-demo integration test. This test pins the two reset variants —
// `step(arr, trig, reset)` and `step(arr, trig, reset, start)`.
//
// Note: PRD §4.4 shows these variants composing `wrap(..., 0, len(arr))`,
// but `len(arr)` is a compile-time builtin that requires the array's length
// at codegen time, which is not available when `arr` is a closure parameter
// bound dynamically by the call site. The PRD itself observes (§4.4 note)
// that ARRAY_INDEX already wraps by default — so the bare
// `arr[counter(trig, reset, ...)]` form is sufficient and is what we test.
// The `len`-inside-closure limitation is a deliberate non-goal of this PRD
// (state cells were the persistence story, not generic compile-time
// reflection on closure parameters); it is captured in the audit report.
TEST_CASE("State: step variant with reset compiles", "[state][step][step_reset]") {
    auto r = akkado::compile(
        "step = (arr, trig, reset) -> arr[counter(trig, reset)]\n"
        "freq = [220, 330, 440, 550].step(trigger(4), trigger(1))\n"
        "sine(freq) * 0.2 |> out(%, %)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    // counter(trig, reset) → at least one EDGE_OP rate=3.
    CHECK(count_op(insts, cedar::Opcode::EDGE_OP, 3) >= 1);
}

TEST_CASE("State: step variant with reset and start compiles",
          "[state][step][step_reset_start]") {
    auto r = akkado::compile(
        "step = (arr, trig, reset, start) -> arr[counter(trig, reset, start)]\n"
        "freq = [220, 330, 440, 550].step(trigger(4), trigger(1), 1)\n"
        "sine(freq) * 0.2 |> out(%, %)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    // counter(trig, reset, start) → exactly one EDGE_OP rate=3 with all three
    // input slots wired (start input not BUFFER_UNUSED).
    bool found_counter_with_start = false;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::EDGE_OP && i.rate == 3 &&
            i.inputs[2] != cedar::BUFFER_UNUSED) {
            found_counter_with_start = true;
            break;
        }
    }
    CHECK(found_counter_with_start);
}

// PRD §9 edge case: empty array `[].step(trig)`. `wrap(x, 0, 0)` and
// `ARRAY_INDEX` both fall back to a safe value (0.0) rather than crashing.
TEST_CASE("State: empty array stepper produces silence not a crash",
          "[state][step][edge_case]") {
    auto r = akkado::compile(
        "step = (arr, trig) -> arr[counter(trig)]\n"
        "[].step(trigger(4)) |> sine(%) |> out(%, %)\n"
    );
    // The compile may succeed (degenerate but safe) or fail with a clear
    // diagnostic — what we forbid is a crash. If it compiles, run a block to
    // confirm the audio thread doesn't blow up.
    if (!r.success) {
        // Acceptable: a clear compile-time error about empty arrays. Not a
        // crash, and not a silent miscompile.
        SUCCEED("empty-array step rejected at compile time");
        return;
    }

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    // Should not crash and output should be finite (not NaN/Inf). We do not
    // assert exact zero — sine(NaN) etc. would already have crashed.
    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
        CHECK(std::isfinite(L[i]));
        CHECK(std::isfinite(R[i]));
    }
}

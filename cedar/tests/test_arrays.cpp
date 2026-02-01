#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/opcodes/utility.hpp"
#include <array>
#include <cmath>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// Helper to create an instruction with array length in rate field
inline Instruction make_array_instruction(Opcode op, std::uint16_t out,
                                          std::uint16_t in0, std::uint8_t rate,
                                          std::uint16_t in1 = BUFFER_UNUSED,
                                          std::uint16_t in2 = BUFFER_UNUSED) {
    Instruction inst{};
    inst.opcode = op;
    inst.out_buffer = out;
    inst.rate = rate;
    inst.inputs[0] = in0;
    inst.inputs[1] = in1;
    inst.inputs[2] = in2;
    inst.inputs[3] = BUFFER_UNUSED;
    inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = 0;
    return inst;
}

// Helper to create ARRAY_PACK instruction with explicit arguments
inline Instruction make_array_pack(std::uint16_t out, std::uint8_t count,
                                   std::uint16_t in0 = BUFFER_UNUSED,
                                   std::uint16_t in1 = BUFFER_UNUSED,
                                   std::uint16_t in2 = BUFFER_UNUSED,
                                   std::uint16_t in3 = BUFFER_UNUSED,
                                   std::uint16_t in4 = BUFFER_UNUSED) {
    Instruction inst{};
    inst.opcode = Opcode::ARRAY_PACK;
    inst.rate = count;
    inst.out_buffer = out;
    inst.inputs[0] = in0;
    inst.inputs[1] = in1;
    inst.inputs[2] = in2;
    inst.inputs[3] = in3;
    inst.inputs[4] = in4;
    inst.state_id = 0;
    return inst;
}

TEST_CASE("ARRAY_PACK creates array from scalar buffers", "[vm][arrays]") {
    VM vm;

    SECTION("pack 3 elements") {
        std::array<Instruction, 4> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 2.0f),
            make_const_instruction(Opcode::PUSH_CONST, 2, 3.0f),
            // Pack 3 elements from buffers 0,1,2 into buffer 3
            make_array_pack(3, 3, 0, 1, 2)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* arr = vm.buffers().get(3);
        CHECK_THAT(arr[0], WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(arr[1], WithinAbs(2.0f, 1e-6f));
        CHECK_THAT(arr[2], WithinAbs(3.0f, 1e-6f));
    }

    SECTION("pack 5 elements (max inline)") {
        std::array<Instruction, 6> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 10.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 20.0f),
            make_const_instruction(Opcode::PUSH_CONST, 2, 30.0f),
            make_const_instruction(Opcode::PUSH_CONST, 3, 40.0f),
            make_const_instruction(Opcode::PUSH_CONST, 4, 50.0f),
            make_array_pack(5, 5, 0, 1, 2, 3, 4)
        };
        vm.load_program(program);

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* arr = vm.buffers().get(5);
        CHECK_THAT(arr[0], WithinAbs(10.0f, 1e-6f));
        CHECK_THAT(arr[1], WithinAbs(20.0f, 1e-6f));
        CHECK_THAT(arr[2], WithinAbs(30.0f, 1e-6f));
        CHECK_THAT(arr[3], WithinAbs(40.0f, 1e-6f));
        CHECK_THAT(arr[4], WithinAbs(50.0f, 1e-6f));
    }
}

TEST_CASE("ARRAY_UNPACK extracts element at compile-time index", "[vm][arrays]") {
    VM vm;

    std::array<Instruction, 5> program = {
        make_const_instruction(Opcode::PUSH_CONST, 0, 100.0f),
        make_const_instruction(Opcode::PUSH_CONST, 1, 200.0f),
        make_const_instruction(Opcode::PUSH_CONST, 2, 300.0f),
        make_array_pack(3, 3, 0, 1, 2),
        // Unpack element at index 1 (should be 200.0)
        make_array_instruction(Opcode::ARRAY_UNPACK, 4, 3, 1)
    };
    vm.load_program(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* result = vm.buffers().get(4);
    // All 128 samples should be filled with the extracted value
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(result[i], WithinAbs(200.0f, 1e-6f));
    }
}

TEST_CASE("ARRAY_INDEX performs per-sample indexing", "[vm][arrays]") {
    VM vm;

    SECTION("wrap mode (rate=0)") {
        // Create array [10, 20, 30] and index buffer with values [0, 1, 2, 3, 0, 1, ...]
        std::array<Instruction, 6> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 10.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 20.0f),
            make_const_instruction(Opcode::PUSH_CONST, 2, 30.0f),
            make_array_pack(3, 3, 0, 1, 2),
            make_const_instruction(Opcode::PUSH_CONST, 4, 3.0f),  // Array length
            // ARRAY_INDEX: arr=3, idx=5, len=4, rate=0 (wrap)
            make_array_instruction(Opcode::ARRAY_INDEX, 6, 3, 0, 5, 4)
        };
        vm.load_program(program);

        // Create index buffer manually: [0, 1, 2, 3, 4, 5, ...]
        float* idx_buf = vm.buffers().get(5);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            idx_buf[i] = static_cast<float>(i);
        }

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(6);
        // With wrap mode, indices should cycle: 0->10, 1->20, 2->30, 3->10, 4->20, ...
        CHECK_THAT(result[0], WithinAbs(10.0f, 1e-6f));
        CHECK_THAT(result[1], WithinAbs(20.0f, 1e-6f));
        CHECK_THAT(result[2], WithinAbs(30.0f, 1e-6f));
        CHECK_THAT(result[3], WithinAbs(10.0f, 1e-6f));  // Wraps to 0
        CHECK_THAT(result[4], WithinAbs(20.0f, 1e-6f));  // Wraps to 1
        CHECK_THAT(result[5], WithinAbs(30.0f, 1e-6f));  // Wraps to 2
    }

    SECTION("clamp mode (rate=1)") {
        std::array<Instruction, 6> program = {
            make_const_instruction(Opcode::PUSH_CONST, 0, 10.0f),
            make_const_instruction(Opcode::PUSH_CONST, 1, 20.0f),
            make_const_instruction(Opcode::PUSH_CONST, 2, 30.0f),
            make_array_pack(3, 3, 0, 1, 2),
            make_const_instruction(Opcode::PUSH_CONST, 4, 3.0f),  // Array length
            // ARRAY_INDEX: rate=1 (clamp)
            make_array_instruction(Opcode::ARRAY_INDEX, 6, 3, 1, 5, 4)
        };
        vm.load_program(program);

        // Create index buffer with out-of-bounds values
        float* idx_buf = vm.buffers().get(5);
        idx_buf[0] = 0.0f;
        idx_buf[1] = 1.0f;
        idx_buf[2] = 2.0f;
        idx_buf[3] = 10.0f;  // Out of bounds high
        idx_buf[4] = -5.0f;  // Out of bounds low

        std::array<float, BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());

        const float* result = vm.buffers().get(6);
        CHECK_THAT(result[0], WithinAbs(10.0f, 1e-6f));
        CHECK_THAT(result[1], WithinAbs(20.0f, 1e-6f));
        CHECK_THAT(result[2], WithinAbs(30.0f, 1e-6f));
        CHECK_THAT(result[3], WithinAbs(30.0f, 1e-6f));  // Clamped to last element
        CHECK_THAT(result[4], WithinAbs(10.0f, 1e-6f));  // Clamped to first element
    }
}

TEST_CASE("ARRAY_LEN fills buffer with array length", "[vm][arrays]") {
    VM vm;

    // ARRAY_LEN with rate=5 should fill buffer with 5.0
    std::array<Instruction, 1> program = {
        make_array_instruction(Opcode::ARRAY_LEN, 0, BUFFER_UNUSED, 5)
    };
    vm.load_program(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* result = vm.buffers().get(0);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(result[i], WithinAbs(5.0f, 1e-6f));
    }
}

TEST_CASE("ARRAY_SUM sums all array elements", "[vm][arrays]") {
    VM vm;

    std::array<Instruction, 5> program = {
        make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
        make_const_instruction(Opcode::PUSH_CONST, 1, 2.0f),
        make_const_instruction(Opcode::PUSH_CONST, 2, 3.0f),
        make_array_pack(3, 3, 0, 1, 2),
        // ARRAY_SUM: arr=3, rate=3 (length)
        make_array_instruction(Opcode::ARRAY_SUM, 4, 3, 3)
    };
    vm.load_program(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* result = vm.buffers().get(4);
    // All samples should be the sum: 1+2+3=6
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK_THAT(result[i], WithinAbs(6.0f, 1e-6f));
    }
}

TEST_CASE("ARRAY_REVERSE reverses array order", "[vm][arrays]") {
    VM vm;

    std::array<Instruction, 5> program = {
        make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
        make_const_instruction(Opcode::PUSH_CONST, 1, 2.0f),
        make_const_instruction(Opcode::PUSH_CONST, 2, 3.0f),
        make_array_pack(3, 3, 0, 1, 2),
        // ARRAY_REVERSE: arr=3, rate=3 (length)
        make_array_instruction(Opcode::ARRAY_REVERSE, 4, 3, 3)
    };
    vm.load_program(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* result = vm.buffers().get(4);
    CHECK_THAT(result[0], WithinAbs(3.0f, 1e-6f));  // Was at index 2
    CHECK_THAT(result[1], WithinAbs(2.0f, 1e-6f));  // Was at index 1
    CHECK_THAT(result[2], WithinAbs(1.0f, 1e-6f));  // Was at index 0
}

TEST_CASE("ARRAY_FILL creates array of repeated value", "[vm][arrays]") {
    VM vm;

    std::array<Instruction, 2> program = {
        make_const_instruction(Opcode::PUSH_CONST, 0, 42.0f),
        // ARRAY_FILL: value=0, rate=4 (length)
        make_array_instruction(Opcode::ARRAY_FILL, 1, 0, 4)
    };
    vm.load_program(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* result = vm.buffers().get(1);
    // First 4 elements should be 42.0
    for (int i = 0; i < 4; ++i) {
        CHECK_THAT(result[i], WithinAbs(42.0f, 1e-6f));
    }
}

TEST_CASE("ARRAY_PUSH appends element to array", "[vm][arrays]") {
    VM vm;

    std::array<Instruction, 5> program = {
        make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
        make_const_instruction(Opcode::PUSH_CONST, 1, 2.0f),
        make_array_pack(2, 2, 0, 1),
        make_const_instruction(Opcode::PUSH_CONST, 3, 3.0f),  // Element to push
        // ARRAY_PUSH: arr=2, elem=3, rate=2 (current length)
        make_array_instruction(Opcode::ARRAY_PUSH, 4, 2, 2, 3)
    };
    vm.load_program(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* result = vm.buffers().get(4);
    CHECK_THAT(result[0], WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(result[1], WithinAbs(2.0f, 1e-6f));
    CHECK_THAT(result[2], WithinAbs(3.0f, 1e-6f));  // New element at index 2
}

TEST_CASE("ARRAY_CONCAT joins two arrays", "[vm][arrays]") {
    VM vm;

    std::array<Instruction, 7> program = {
        // Array A: [1, 2]
        make_const_instruction(Opcode::PUSH_CONST, 0, 1.0f),
        make_const_instruction(Opcode::PUSH_CONST, 1, 2.0f),
        make_array_pack(2, 2, 0, 1),
        // Array B: [3, 4]
        make_const_instruction(Opcode::PUSH_CONST, 3, 3.0f),
        make_const_instruction(Opcode::PUSH_CONST, 4, 4.0f),
        make_array_pack(5, 2, 3, 4),
        // len_b buffer
        make_const_instruction(Opcode::PUSH_CONST, 6, 2.0f),
    };

    // Add ARRAY_CONCAT separately since it has a complex setup
    Instruction concat{};
    concat.opcode = Opcode::ARRAY_CONCAT;
    concat.out_buffer = 7;
    concat.rate = 2;  // len_a
    concat.inputs[0] = 2;  // arr_a
    concat.inputs[1] = 5;  // arr_b
    concat.inputs[2] = 6;  // len_b buffer
    concat.inputs[3] = BUFFER_UNUSED;
    concat.inputs[4] = BUFFER_UNUSED;
    concat.state_id = 0;

    std::vector<Instruction> full_program(program.begin(), program.end());
    full_program.push_back(concat);

    vm.load_program(full_program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    const float* result = vm.buffers().get(7);
    CHECK_THAT(result[0], WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(result[1], WithinAbs(2.0f, 1e-6f));
    CHECK_THAT(result[2], WithinAbs(3.0f, 1e-6f));
    CHECK_THAT(result[3], WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("ARRAY_SLICE extracts subarray", "[vm][arrays]") {
    VM vm;

    std::array<Instruction, 8> program = {
        // Array: [10, 20, 30, 40, 50]
        make_const_instruction(Opcode::PUSH_CONST, 0, 10.0f),
        make_const_instruction(Opcode::PUSH_CONST, 1, 20.0f),
        make_const_instruction(Opcode::PUSH_CONST, 2, 30.0f),
        make_const_instruction(Opcode::PUSH_CONST, 3, 40.0f),
        make_const_instruction(Opcode::PUSH_CONST, 4, 50.0f),
        make_array_pack(5, 5, 0, 1, 2, 3, 4),
        // Start and end indices
        make_const_instruction(Opcode::PUSH_CONST, 6, 1.0f),  // start
        make_const_instruction(Opcode::PUSH_CONST, 7, 4.0f),  // end (exclusive)
    };

    // Add ARRAY_SLICE
    Instruction slice{};
    slice.opcode = Opcode::ARRAY_SLICE;
    slice.out_buffer = 8;
    slice.rate = 5;  // source array length
    slice.inputs[0] = 5;  // arr
    slice.inputs[1] = 6;  // start
    slice.inputs[2] = 7;  // end
    slice.inputs[3] = BUFFER_UNUSED;
    slice.inputs[4] = BUFFER_UNUSED;
    slice.state_id = 0;

    std::vector<Instruction> full_program(program.begin(), program.end());
    full_program.push_back(slice);

    vm.load_program(full_program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    // Slice should be [20, 30, 40] (indices 1, 2, 3)
    const float* result = vm.buffers().get(8);
    CHECK_THAT(result[0], WithinAbs(20.0f, 1e-6f));
    CHECK_THAT(result[1], WithinAbs(30.0f, 1e-6f));
    CHECK_THAT(result[2], WithinAbs(40.0f, 1e-6f));
}

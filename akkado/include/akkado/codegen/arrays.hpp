#pragma once

// Helper functions for array higher-order function code generation
// These reduce boilerplate in map(), sum(), fold(), zipWith(), etc.

#include "akkado/ast.hpp"
#include "akkado/codegen.hpp"
#include "helpers.hpp"
#include <cedar/vm/instruction.hpp>
#include <vector>

namespace akkado {
namespace codegen {

// Emit a zero constant buffer (used for empty array results)
// Returns buffer index or BUFFER_UNUSED on failure
[[gnu::always_inline]]
inline std::uint16_t emit_zero(
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions
) {
    std::uint16_t out = buffers.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::PUSH_CONST;
    inst.out_buffer = out;
    set_unused_inputs(inst);
    encode_const_value(inst, 0.0f);
    instructions.push_back(inst);

    return out;
}

// Result of extracting call arguments
struct CallArgs {
    std::vector<NodeIndex> nodes;
    bool valid = true;
};

// Extract N arguments from a Call node, unwrapping Argument wrappers
// Returns empty nodes vector with valid=false if argument count doesn't match expected
[[gnu::always_inline]]
inline CallArgs extract_call_args(
    const AstArena& arena,
    NodeIndex first_arg,
    std::size_t expected_min,
    std::size_t expected_max = 0
) {
    if (expected_max == 0) expected_max = expected_min;

    CallArgs result;
    NodeIndex arg = first_arg;

    while (arg != NULL_NODE) {
        NodeIndex unwrapped = unwrap_argument(arena, arg);
        result.nodes.push_back(unwrapped);
        arg = arena[arg].next_sibling;
    }

    if (result.nodes.size() < expected_min || result.nodes.size() > expected_max) {
        result.valid = false;
    }

    return result;
}

// Finalize multi-buffer array result:
// - Empty vector: emit zero constant
// - Single element: return as Signal
// - Multiple elements: return as Array
// Returns a TypedValue for the result
[[gnu::always_inline]]
inline TypedValue finalize_array_result(
    NodeIndex node,
    std::vector<std::uint16_t> result_buffers,
    std::unordered_map<NodeIndex, TypedValue>& node_types,
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions
) {
    if (result_buffers.empty()) {
        std::uint16_t zero = emit_zero(buffers, instructions);
        auto tv = TypedValue::signal(zero);
        node_types[node] = tv;
        return tv;
    }

    if (result_buffers.size() == 1) {
        auto tv = TypedValue::signal(result_buffers[0]);
        node_types[node] = tv;
        return tv;
    }

    std::uint16_t first_buf = result_buffers[0];
    std::vector<TypedValue> elements;
    elements.reserve(result_buffers.size());
    for (auto buf : result_buffers) {
        elements.push_back(TypedValue::signal(buf));
    }
    auto tv = TypedValue::make_array(std::move(elements), first_buf);
    node_types[node] = tv;
    return tv;
}

// Get input buffers from a node (handles both single and multi-buffer sources)
// Checks the node_types map for Array typed values
[[gnu::always_inline]]
inline std::vector<std::uint16_t> get_input_buffers(
    NodeIndex array_node,
    std::uint16_t single_buf,
    const std::unordered_map<NodeIndex, TypedValue>& node_types
) {
    auto it = node_types.find(array_node);
    if (it != node_types.end() && it->second.type == ValueType::Array && it->second.array) {
        std::vector<std::uint16_t> bufs;
        bufs.reserve(it->second.array->elements.size());
        for (const auto& elem : it->second.array->elements) {
            bufs.push_back(elem.buffer);
        }
        if (bufs.size() > 1) return bufs;
    }
    return {single_buf};
}

} // namespace codegen
} // namespace akkado

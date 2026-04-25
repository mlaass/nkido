// User state cells (state/get/set) — Phase 3 of
// prd-userspace-state-and-edge-primitives.md.
//
// state(init) lowers to STATE_OP rate=0 and returns a StateCell TypedValue
// that carries the slot's state_id (FNV-1a path-hash, identical scheme to
// every other stateful builtin). .get() and .set() consume that handle and
// emit STATE_OP rate=1 (load) and rate=2 (store) respectively.
//
// state/get/set are name-reserved at the parser level — users cannot define
// closures with these names. See parser.cpp.

#include "akkado/codegen.hpp"
#include "akkado/builtins.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>

namespace akkado {

namespace {

// Walk past an optional Argument wrapper to the underlying expression node.
NodeIndex unwrap_argument(const AstArena& arena, NodeIndex idx) {
    if (idx == NULL_NODE) return NULL_NODE;
    const Node& n = arena[idx];
    if (n.type == NodeType::Argument) return n.first_child;
    return idx;
}

}  // namespace

TypedValue CodeGenerator::handle_state_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E120", "state() requires exactly 1 argument (initial value)", n.location);
        return TypedValue::error_val();
    }
    NodeIndex init_node = unwrap_argument(ast_->arena, arg);
    if (init_node == NULL_NODE) {
        error("E120", "state() requires an initial value", n.location);
        return TypedValue::error_val();
    }

    // Allocate this call site's slot via the same path-hash mechanism used by
    // every other stateful builtin. Each `state(...)` source position gets a
    // distinct state_id; inside a closure called from multiple sites, the
    // surrounding push_path("closure_name#N") gives each invocation its own
    // slot — which is exactly the semantic users expect for `make_counter()`.
    std::uint32_t count = call_counters_["state"]++;
    std::string unique_name = std::string("state#") + std::to_string(count);
    push_path(unique_name);
    std::uint32_t state_id = compute_state_id();

    // Visit the init expression to get its buffer.
    TypedValue init_tv = visit(init_node);
    if (init_tv.error) {
        pop_path();
        return TypedValue::error_val();
    }
    if (init_tv.type != ValueType::Signal && init_tv.type != ValueType::Number) {
        error("E122", "state() initial value must be a number or signal", n.location);
        pop_path();
        return TypedValue::error_val();
    }

    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::STATE_OP;
    inst.rate = 0;  // init mode
    inst.out_buffer = out;
    inst.inputs[0] = init_tv.buffer;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = state_id;
    emit(inst);

    pop_path();
    return cache_and_return(node, TypedValue::state_cell(state_id, out));
}

TypedValue CodeGenerator::handle_get_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E120", "get() requires exactly 1 argument (a state cell)", n.location);
        return TypedValue::error_val();
    }
    NodeIndex cell_node = unwrap_argument(ast_->arena, arg);
    TypedValue cell_tv = visit(cell_node);
    if (cell_tv.error) return TypedValue::error_val();
    if (cell_tv.type != ValueType::StateCell) {
        error("E122",
              std::string("get() requires a state cell, got ") +
              value_type_name(cell_tv.type),
              n.location);
        return TypedValue::error_val();
    }

    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::STATE_OP;
    inst.rate = 1;  // load mode
    inst.out_buffer = out;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = cell_tv.cell_state_id;
    emit(inst);

    return cache_and_return(node, TypedValue::signal(out));
}

TypedValue CodeGenerator::handle_set_call(NodeIndex node, const Node& n) {
    NodeIndex arg1 = n.first_child;
    if (arg1 == NULL_NODE) {
        error("E120", "set() requires 2 arguments (cell, value)", n.location);
        return TypedValue::error_val();
    }
    NodeIndex arg2 = ast_->arena[arg1].next_sibling;
    if (arg2 == NULL_NODE) {
        error("E120", "set() requires 2 arguments (cell, value)", n.location);
        return TypedValue::error_val();
    }

    NodeIndex cell_node = unwrap_argument(ast_->arena, arg1);
    NodeIndex value_node = unwrap_argument(ast_->arena, arg2);

    TypedValue cell_tv = visit(cell_node);
    if (cell_tv.error) return TypedValue::error_val();
    if (cell_tv.type != ValueType::StateCell) {
        error("E122",
              std::string("set() first argument must be a state cell, got ") +
              value_type_name(cell_tv.type),
              n.location);
        return TypedValue::error_val();
    }

    TypedValue value_tv = visit(value_node);
    if (value_tv.error) return TypedValue::error_val();
    if (value_tv.type != ValueType::Signal && value_tv.type != ValueType::Number) {
        error("E122", "set() value must be a number or signal", n.location);
        return TypedValue::error_val();
    }

    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::STATE_OP;
    inst.rate = 2;  // store mode
    inst.out_buffer = out;
    inst.inputs[0] = value_tv.buffer;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = cell_tv.cell_state_id;
    emit(inst);

    // Returning a Signal lets users chain set() in expression position:
    //   arr[s.set(s.get() + 1)] — advance and use the new index in one shot.
    return cache_and_return(node, TypedValue::signal(out));
}

}  // namespace akkado

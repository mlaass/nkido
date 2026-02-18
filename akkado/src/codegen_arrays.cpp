// Array higher-order function codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/const_eval.hpp"
#include <algorithm>
#include <cmath>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::emit_zero;
using codegen::emit_push_const;
using codegen::extract_call_args;
using codegen::get_input_buffers;

// Try to resolve an AST node to a compile-time scalar constant.
// Accepts NumberLit directly, or Identifier referencing a const variable.
static std::optional<double> resolve_const_scalar(
    const AstArena& arena, NodeIndex node, const SymbolTable& symbols) {

    if (node == NULL_NODE) return std::nullopt;
    const Node& n = arena[node];

    if (n.type == NodeType::NumberLit) {
        return n.as_number();
    }

    if (n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        }
        if (!name.empty()) {
            auto sym = symbols.lookup(name);
            if (sym && sym->is_const && sym->const_value.has_value()) {
                if (std::holds_alternative<double>(*sym->const_value)) {
                    return std::get<double>(*sym->const_value);
                }
            }
        }
    }

    return std::nullopt;
}

// Multi-buffer registration
std::uint16_t CodeGenerator::register_multi_buffer(NodeIndex node, std::vector<std::uint16_t> buffers) {
    if (buffers.empty()) return BufferAllocator::BUFFER_UNUSED;
    multi_buffers_[node] = std::move(buffers);
    return multi_buffers_[node][0];
}

bool CodeGenerator::is_multi_buffer(NodeIndex node) const {
    // First check node-based tracking
    auto it = multi_buffers_.find(node);
    if (it != multi_buffers_.end() && it->second.size() > 1) {
        return true;
    }
    // Fallback: check if the node's buffer is a stereo left channel
    auto buf_it = node_buffers_.find(node);
    if (buf_it != node_buffers_.end()) {
        return is_stereo_buffer(buf_it->second);
    }
    return false;
}

std::vector<std::uint16_t> CodeGenerator::get_multi_buffers(NodeIndex node) const {
    // First check node-based tracking
    auto it = multi_buffers_.find(node);
    if (it != multi_buffers_.end()) return it->second;

    // Fallback: check buffer-based stereo tracking
    auto buf_it = node_buffers_.find(node);
    if (buf_it != node_buffers_.end() && buf_it->second != BufferAllocator::BUFFER_UNUSED) {
        // Check if this is a stereo buffer
        auto pair_it = stereo_buffer_pairs_.find(buf_it->second);
        if (pair_it != stereo_buffer_pairs_.end()) {
            return {buf_it->second, pair_it->second};
        }
        return {buf_it->second};
    }
    return {};
}

// Apply lambda to single buffer
std::uint16_t CodeGenerator::apply_lambda(NodeIndex lambda_node, std::uint16_t arg_buf) {
    const Node& lambda = ast_->arena[lambda_node];
    if (lambda.type != NodeType::Closure) {
        error("E130", "map() second argument must be a lambda (fn => expr)", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::vector<std::string> param_names;
    NodeIndex child = lambda.first_child;
    NodeIndex body = NULL_NODE;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_names.push_back(child_node.as_closure_param().name);
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_names.push_back(child_node.as_identifier());
            } else {
                body = child;
                break;
            }
        } else {
            body = child;
            break;
        }
        child = ast_->arena[child].next_sibling;
    }

    if (body == NULL_NODE) {
        error("E131", "Lambda has no body", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (param_names.empty()) {
        error("E132", "Lambda must have at least one parameter", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    symbols_->define_variable(param_names[0], arg_buf);

    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    std::uint16_t result = visit(body);

    node_buffers_ = std::move(saved_node_buffers);
    symbols_->pop_scope();

    return result;
}

// Resolve function argument (lambda, variable, or function name)
std::optional<FunctionRef> CodeGenerator::resolve_function_arg(NodeIndex func_node) {
    const Node& n = ast_->arena[func_node];

    if (n.type == NodeType::Closure) {
        FunctionRef ref{};
        ref.closure_node = func_node;
        ref.is_user_function = false;

        // Count total children to know how many are params (all except last = body)
        std::size_t child_count = 0;
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            child_count++;
            child = ast_->arena[child].next_sibling;
        }

        // All children except the last one are parameters
        std::size_t param_count = child_count > 0 ? child_count - 1 : 0;
        child = n.first_child;
        for (std::size_t i = 0; i < param_count && child != NULL_NODE; ++i) {
            const Node& child_node = ast_->arena[child];
            if (child_node.type == NodeType::Identifier) {
                FunctionParamInfo param;
                if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                    param.name = child_node.as_closure_param().name;
                    param.default_value = child_node.as_closure_param().default_value;
                } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                    param.name = child_node.as_identifier();
                }
                ref.params.push_back(std::move(param));
            }
            child = ast_->arena[child].next_sibling;
        }
        return ref;
    }

    if (n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        } else {
            return std::nullopt;
        }

        auto sym = symbols_->lookup(name);
        if (!sym) return std::nullopt;

        if (sym->kind == SymbolKind::FunctionValue) {
            return sym->function_ref;
        }
        if (sym->kind == SymbolKind::UserFunction) {
            FunctionRef ref{};
            ref.is_user_function = true;
            ref.user_function_name = sym->name;
            ref.params = sym->user_function.params;
            ref.closure_node = sym->user_function.body_node;
            return ref;
        }
    }

    return std::nullopt;
}

// Apply unary function ref
std::uint16_t CodeGenerator::apply_function_ref(const FunctionRef& ref, std::uint16_t arg_buf,
                                                  SourceLocation loc) {
    if (ref.params.empty()) {
        error("E132", "Function must have at least one parameter", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    for (const auto& capture : ref.captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }
    symbols_->define_variable(ref.params[0].name, arg_buf);

    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    if (ref.is_user_function) {
        if (ref.closure_node != NULL_NODE) result = visit(ref.closure_node);
    } else {
        const Node& closure = ast_->arena[ref.closure_node];
        NodeIndex child = closure.first_child;
        NodeIndex body = NULL_NODE;

        while (child != NULL_NODE) {
            const Node& child_node = ast_->arena[child];
            if (child_node.type == NodeType::Identifier &&
                (std::holds_alternative<Node::ClosureParamData>(child_node.data) ||
                 std::holds_alternative<Node::IdentifierData>(child_node.data))) {
                child = child_node.next_sibling;
                continue;
            }
            body = child;
            break;
        }
        if (body != NULL_NODE) result = visit(body);
    }

    node_buffers_ = std::move(saved_node_buffers);
    symbols_->pop_scope();

    return result;
}

// Apply binary function ref
std::uint16_t CodeGenerator::apply_binary_function_ref(const FunctionRef& ref,
                                                        std::uint16_t arg_buf1,
                                                        std::uint16_t arg_buf2,
                                                        SourceLocation loc) {
    if (ref.params.size() < 2) {
        error("E140", "Binary function must have at least two parameters", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    for (const auto& capture : ref.captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }
    symbols_->define_variable(ref.params[0].name, arg_buf1);
    symbols_->define_variable(ref.params[1].name, arg_buf2);

    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    if (ref.is_user_function) {
        if (ref.closure_node != NULL_NODE) result = visit(ref.closure_node);
    } else {
        const Node& closure = ast_->arena[ref.closure_node];
        NodeIndex child = closure.first_child;
        NodeIndex body = NULL_NODE;

        while (child != NULL_NODE) {
            const Node& child_node = ast_->arena[child];
            if (child_node.type == NodeType::Identifier &&
                (std::holds_alternative<Node::ClosureParamData>(child_node.data) ||
                 std::holds_alternative<Node::IdentifierData>(child_node.data))) {
                child = child_node.next_sibling;
                continue;
            }
            body = child;
            break;
        }
        if (body != NULL_NODE) result = visit(body);
    }

    node_buffers_ = std::move(saved_node_buffers);
    symbols_->pop_scope();

    return result;
}

// Finalize array result (helper)
static std::uint16_t finalize_result(
    CodeGenerator* cg,
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions,
    NodeIndex node,
    std::vector<std::uint16_t> result_buffers,
    std::unordered_map<NodeIndex, std::uint16_t>& node_buffers,
    std::unordered_map<NodeIndex, std::vector<std::uint16_t>>& multi_buffers
) {
    if (result_buffers.empty()) {
        std::uint16_t zero = emit_zero(buffers, instructions);
        node_buffers[node] = zero;
        return zero;
    }
    if (result_buffers.size() == 1) {
        node_buffers[node] = result_buffers[0];
        return result_buffers[0];
    }
    std::uint16_t first_buf = result_buffers[0];
    multi_buffers[node] = std::move(result_buffers);
    node_buffers[node] = first_buf;
    return first_buf;
}

// map(array, fn)
std::uint16_t CodeGenerator::handle_map_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E133", "map() requires 2 arguments: map(array, fn)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto func_ref = resolve_function_arg(args.nodes[1]);
    if (!func_ref) {
        error("E130", "map() second argument must be a function", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        push_path("map#" + std::to_string(call_counters_["map"]++));
        push_path("elem0");
        std::uint16_t result = apply_function_ref(*func_ref, array_buf, n.location);
        pop_path();
        pop_path();
        node_buffers_[node] = result;
        return result;
    }

    auto element_buffers = get_multi_buffers(args.nodes[0]);
    std::vector<std::uint16_t> result_buffers;

    push_path("map#" + std::to_string(call_counters_["map"]++));
    for (std::size_t i = 0; i < element_buffers.size(); ++i) {
        push_path("elem" + std::to_string(i));
        result_buffers.push_back(apply_function_ref(*func_ref, element_buffers[i], n.location));
        pop_path();
    }
    pop_path();

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// sum(array)
std::uint16_t CodeGenerator::handle_sum_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E134", "sum() requires 1 argument: sum(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.empty()) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }
    if (buffers.size() == 1) {
        node_buffers_[node] = buffers[0];
        return buffers[0];
    }

    std::uint16_t result = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        std::uint16_t sum_buf = buffers_.allocate();
        if (sum_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction add_inst{};
        add_inst.opcode = cedar::Opcode::ADD;
        add_inst.out_buffer = sum_buf;
        add_inst.inputs[0] = result;
        add_inst.inputs[1] = buffers[i];
        add_inst.inputs[2] = 0xFFFF;
        add_inst.inputs[3] = 0xFFFF;
        add_inst.state_id = 0;
        emit(add_inst);

        result = sum_buf;
    }

    node_buffers_[node] = result;
    return result;
}

// fold(array, fn, init)
std::uint16_t CodeGenerator::handle_fold_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E142", "fold() requires 3 arguments: fold(array, fn, init)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto func_ref = resolve_function_arg(args.nodes[1]);
    if (!func_ref) {
        error("E143", "fold() second argument must be a binary function", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);
    std::uint16_t init_buf = visit(args.nodes[2]);

    auto buffers = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{array_buf};

    if (buffers.empty()) {
        node_buffers_[node] = init_buf;
        return init_buf;
    }

    push_path("fold#" + std::to_string(call_counters_["fold"]++));
    std::uint16_t result = init_buf;
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        push_path("step" + std::to_string(i));
        result = apply_binary_function_ref(*func_ref, result, buffers[i], n.location);
        pop_path();
    }
    pop_path();

    node_buffers_[node] = result;
    return result;
}

// zipWith(a, b, fn)
std::uint16_t CodeGenerator::handle_zipWith_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E144", "zipWith() requires 3 arguments: zipWith(a, b, fn)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto func_ref = resolve_function_arg(args.nodes[2]);
    if (!func_ref) {
        error("E145", "zipWith() third argument must be a binary function", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t buf_a = visit(args.nodes[0]);
    std::uint16_t buf_b = visit(args.nodes[1]);

    auto buffers_a = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{buf_a};
    auto buffers_b = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{buf_b};

    std::size_t len = std::min(buffers_a.size(), buffers_b.size());
    if (len == 0) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    push_path("zipWith#" + std::to_string(call_counters_["zipWith"]++));
    std::vector<std::uint16_t> result_buffers;
    for (std::size_t i = 0; i < len; ++i) {
        push_path("elem" + std::to_string(i));
        result_buffers.push_back(apply_binary_function_ref(*func_ref, buffers_a[i], buffers_b[i], n.location));
        pop_path();
    }
    pop_path();

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// zip(a, b) - interleave
std::uint16_t CodeGenerator::handle_zip_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E146", "zip() requires 2 arguments: zip(a, b)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t buf_a = visit(args.nodes[0]);
    std::uint16_t buf_b = visit(args.nodes[1]);

    auto buffers_a = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{buf_a};
    auto buffers_b = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{buf_b};

    std::size_t len = std::min(buffers_a.size(), buffers_b.size());
    std::vector<std::uint16_t> result_buffers;
    result_buffers.reserve(len * 2);

    for (std::size_t i = 0; i < len; ++i) {
        result_buffers.push_back(buffers_a[i]);
        result_buffers.push_back(buffers_b[i]);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// take(n, array)
std::uint16_t CodeGenerator::handle_take_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E147", "take() requires 2 arguments: take(n, array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E148", "take() first argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[1]);

    auto buffers = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{array_buf};

    count = std::min(count, buffers.size());
    std::vector<std::uint16_t> result_buffers(buffers.begin(), buffers.begin() + count);

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// drop(n, array)
std::uint16_t CodeGenerator::handle_drop_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E149", "drop() requires 2 arguments: drop(n, array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E150", "drop() first argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[1]);

    auto buffers = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{array_buf};

    count = std::min(count, buffers.size());
    std::vector<std::uint16_t> result_buffers(buffers.begin() + count, buffers.end());

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// reverse(array)
std::uint16_t CodeGenerator::handle_reverse_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E151", "reverse() requires 1 argument: reverse(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    std::reverse(buffers.begin(), buffers.end());

    std::uint16_t first_buf = register_multi_buffer(node, std::move(buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

// range(start, end)
std::uint16_t CodeGenerator::handle_range_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E152", "range() requires 2 arguments: range(start, end)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto start_val = resolve_const_scalar(ast_->arena, args.nodes[0], *symbols_);
    auto end_val = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);

    if (!start_val || !end_val) {
        error("E153", "range() arguments must be compile-time constants", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    int start = static_cast<int>(*start_val);
    int end = static_cast<int>(*end_val);

    std::vector<std::uint16_t> result_buffers;
    int step = (start <= end) ? 1 : -1;

    for (int i = start; i != end; i += step) {
        std::uint16_t buf = buffers_.allocate();
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction inst{};
        inst.opcode = cedar::Opcode::PUSH_CONST;
        inst.out_buffer = buf;
        inst.inputs[0] = 0xFFFF;
        inst.inputs[1] = 0xFFFF;
        inst.inputs[2] = 0xFFFF;
        inst.inputs[3] = 0xFFFF;
        encode_const_value(inst, static_cast<float>(i));
        emit(inst);

        result_buffers.push_back(buf);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// repeat(value, n)
std::uint16_t CodeGenerator::handle_repeat_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E154", "repeat() requires 2 arguments: repeat(value, n)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& n_val = ast_->arena[args.nodes[1]];
    if (n_val.type != NodeType::NumberLit) {
        error("E155", "repeat() second argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t value_buf = visit(args.nodes[0]);

    if (count == 0) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }
    if (count == 1) {
        node_buffers_[node] = value_buf;
        return value_buf;
    }

    std::vector<std::uint16_t> result_buffers(count, value_buf);
    std::uint16_t first_buf = register_multi_buffer(node, std::move(result_buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

// spread(n, source) - force specific voice count (pad with zeros or truncate)
std::uint16_t CodeGenerator::handle_spread_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E178", "spread() requires 2 arguments: spread(n, source)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // First argument (n) must be a compile-time constant
    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E179", "spread() first argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    int target_count = static_cast<int>(n_val.as_number());
    if (target_count < 1 || target_count > 128) {
        error("E180", "spread() count must be between 1 and 128", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Visit the source to get its buffers
    std::uint16_t source_buf = visit(args.nodes[1]);

    // Get source buffers (multi-buffer or single)
    std::vector<std::uint16_t> source_buffers;
    if (is_multi_buffer(args.nodes[1])) {
        source_buffers = get_multi_buffers(args.nodes[1]);
    } else {
        source_buffers.push_back(source_buf);
    }

    std::size_t target = static_cast<std::size_t>(target_count);
    std::vector<std::uint16_t> result_buffers;
    result_buffers.reserve(target);

    if (source_buffers.size() >= target) {
        // Truncate: take first target_count elements
        for (std::size_t i = 0; i < target; ++i) {
            result_buffers.push_back(source_buffers[i]);
        }
    } else {
        // Pad: copy all source buffers, then pad with zeros
        for (auto buf : source_buffers) {
            result_buffers.push_back(buf);
        }
        // Pad remaining slots with zero buffers
        for (std::size_t i = source_buffers.size(); i < target; ++i) {
            std::uint16_t zero_buf = emit_zero(buffers_, instructions_);
            if (zero_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }
            result_buffers.push_back(zero_buf);
        }
    }

    // If single result, just return it
    if (result_buffers.size() == 1) {
        node_buffers_[node] = result_buffers[0];
        return result_buffers[0];
    }

    // Register as multi-buffer result
    std::uint16_t first_buf = register_multi_buffer(node, std::move(result_buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

// Handles len(arr) calls - returns compile-time array length
std::uint16_t CodeGenerator::handle_len_call(NodeIndex node, const Node& n) {
    // Get the argument
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E120", "len() requires exactly 1 argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Unwrap Argument node if present
    const Node& arg_node = ast_->arena[arg];
    NodeIndex arr_node = arg;
    if (arg_node.type == NodeType::Argument) {
        arr_node = arg_node.first_child;
    }

    if (arr_node == NULL_NODE) {
        error("E120", "len() requires an array argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& arr = ast_->arena[arr_node];

    // Count elements based on node type
    std::size_t length = 0;

    if (arr.type == NodeType::ArrayLit) {
        // Count children of array literal
        NodeIndex elem = arr.first_child;
        while (elem != NULL_NODE) {
            length++;
            elem = ast_->arena[elem].next_sibling;
        }
    } else if (arr.type == NodeType::Identifier) {
        // Look up the symbol to see if it's a known array
        const std::string& name = arr.as_identifier();
        auto sym = symbols_->lookup(name);
        if (sym && sym->kind == SymbolKind::Array) {
            length = sym->array.element_count;
        } else {
            error("E141", "len() requires an array, but '" + name + "' is not an array",
                  arr.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
    } else {
        error("E122", "len() argument must be an array", arr.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit the length as a constant
    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::PUSH_CONST;
    inst.out_buffer = out;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;

    float len_value = static_cast<float>(length);
    encode_const_value(inst, len_value);
    emit(inst);

    node_buffers_[node] = out;
    return out;
}

// ============================================================================
// Binary operation broadcasting
// ============================================================================

// Map function name to opcode
static cedar::Opcode binary_name_to_opcode(std::string_view func_name) {
    if (func_name == "add") return cedar::Opcode::ADD;
    if (func_name == "sub") return cedar::Opcode::SUB;
    if (func_name == "mul") return cedar::Opcode::MUL;
    if (func_name == "div") return cedar::Opcode::DIV;
    if (func_name == "pow") return cedar::Opcode::POW;
    return cedar::Opcode::NOP;
}

// Emit a single binary operation instruction
static std::uint16_t emit_binary_op(
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions,
    cedar::Opcode op,
    std::uint16_t lhs,
    std::uint16_t rhs
) {
    std::uint16_t out = buffers.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction inst{};
    inst.opcode = op;
    inst.out_buffer = out;
    inst.inputs[0] = lhs;
    inst.inputs[1] = rhs;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = 0;
    instructions.push_back(inst);

    return out;
}

std::uint16_t CodeGenerator::handle_binary_op_call(NodeIndex node, const Node& n) {
    std::string func_name;
    if (std::holds_alternative<Node::IdentifierData>(n.data)) {
        func_name = n.as_identifier();
    } else {
        error("E160", "Invalid binary operation call", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Opcode op = binary_name_to_opcode(func_name);
    if (op == cedar::Opcode::NOP) {
        error("E161", "Unknown binary operation: " + func_name, n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E162", func_name + "() requires 2 arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t buf_a = visit(args.nodes[0]);
    std::uint16_t buf_b = visit(args.nodes[1]);

    bool a_multi = is_multi_buffer(args.nodes[0]);
    bool b_multi = is_multi_buffer(args.nodes[1]);

    // Neither is array - standard scalar operation
    if (!a_multi && !b_multi) {
        std::uint16_t result = emit_binary_op(buffers_, instructions_, op, buf_a, buf_b);
        node_buffers_[node] = result;
        return result;
    }

    auto buffers_a = a_multi ? get_multi_buffers(args.nodes[0])
                             : std::vector<std::uint16_t>{buf_a};
    auto buffers_b = b_multi ? get_multi_buffers(args.nodes[1])
                             : std::vector<std::uint16_t>{buf_b};

    // Broadcasting: use the larger size, cycle the smaller array
    std::size_t len = std::max(buffers_a.size(), buffers_b.size());
    std::vector<std::uint16_t> result_buffers;

    for (std::size_t i = 0; i < len; ++i) {
        std::uint16_t a = buffers_a[i % buffers_a.size()];
        std::uint16_t b = buffers_b[i % buffers_b.size()];
        std::uint16_t res = emit_binary_op(buffers_, instructions_, op, a, b);
        if (res == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        result_buffers.push_back(res);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// ============================================================================
// Array reduction operations
// ============================================================================

// product(array) - multiply all elements
std::uint16_t CodeGenerator::handle_product_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E163", "product() requires 1 argument: product(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - return as-is
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.empty()) {
        // Product of empty array is 1.0 (multiplicative identity)
        std::uint16_t one = emit_push_const(buffers_, instructions_, 1.0f);
        node_buffers_[node] = one;
        return one;
    }
    if (buffers.size() == 1) {
        node_buffers_[node] = buffers[0];
        return buffers[0];
    }

    std::uint16_t result = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        result = emit_binary_op(buffers_, instructions_, cedar::Opcode::MUL, result, buffers[i]);
        if (result == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
    }

    node_buffers_[node] = result;
    return result;
}

// mean(array) - average of elements
std::uint16_t CodeGenerator::handle_mean_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E164", "mean() requires 1 argument: mean(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - return as-is
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.empty()) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }
    if (buffers.size() == 1) {
        node_buffers_[node] = buffers[0];
        return buffers[0];
    }

    // Sum all elements
    std::uint16_t sum_buf = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        sum_buf = emit_binary_op(buffers_, instructions_, cedar::Opcode::ADD, sum_buf, buffers[i]);
        if (sum_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
    }

    // Divide by length
    std::uint16_t len_buf = emit_push_const(buffers_, instructions_, static_cast<float>(buffers.size()));
    if (len_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t result = emit_binary_op(buffers_, instructions_, cedar::Opcode::DIV, sum_buf, len_buf);
    node_buffers_[node] = result;
    return result;
}

// min/max with array support - either binary or reduction
std::uint16_t CodeGenerator::handle_minmax_call(NodeIndex node, const Node& n) {
    std::string func_name;
    if (std::holds_alternative<Node::IdentifierData>(n.data)) {
        func_name = n.as_identifier();
    }

    cedar::Opcode op = (func_name == "min") ? cedar::Opcode::MIN : cedar::Opcode::MAX;

    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);
    if (!args.valid || args.nodes.empty()) {
        error("E165", func_name + "() requires 1 or 2 arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Two arguments: standard binary min/max
    if (args.nodes.size() == 2) {
        std::uint16_t buf_a = visit(args.nodes[0]);
        std::uint16_t buf_b = visit(args.nodes[1]);
        std::uint16_t result = emit_binary_op(buffers_, instructions_, op, buf_a, buf_b);
        node_buffers_[node] = result;
        return result;
    }

    // Single argument: reduction over array
    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - return as-is
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.empty()) {
        // min/max of empty array - return 0
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }
    if (buffers.size() == 1) {
        node_buffers_[node] = buffers[0];
        return buffers[0];
    }

    std::uint16_t result = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        result = emit_binary_op(buffers_, instructions_, op, result, buffers[i]);
        if (result == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
    }

    node_buffers_[node] = result;
    return result;
}

// ============================================================================
// Array transformation operations
// ============================================================================

// rotate(array, n) - rotate elements by n positions
std::uint16_t CodeGenerator::handle_rotate_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E166", "rotate() requires 2 arguments: rotate(array, n)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // n must be a literal
    const Node& n_val = ast_->arena[args.nodes[1]];
    if (n_val.type != NodeType::NumberLit) {
        error("E167", "rotate() second argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    int offset = static_cast<int>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - return as-is
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.empty() || buffers.size() == 1) {
        std::uint16_t first_buf = buffers.empty() ? emit_zero(buffers_, instructions_) : buffers[0];
        node_buffers_[node] = first_buf;
        return first_buf;
    }

    int len = static_cast<int>(buffers.size());
    // Normalize offset (positive = rotate right, negative = rotate left)
    offset = ((offset % len) + len) % len;

    std::vector<std::uint16_t> result;
    for (int i = 0; i < len; ++i) {
        // rotate right by offset: element at i comes from (i - offset + len) % len
        int src_idx = (i - offset + len) % len;
        result.push_back(buffers[static_cast<std::size_t>(src_idx)]);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result),
                           node_buffers_, multi_buffers_);
}

// shuffle(array) - deterministic random permutation
std::uint16_t CodeGenerator::handle_shuffle_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E168", "shuffle() requires 1 argument: shuffle(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.size() <= 1) {
        std::uint16_t first_buf = buffers.empty() ? emit_zero(buffers_, instructions_) : buffers[0];
        node_buffers_[node] = first_buf;
        return first_buf;
    }

    // Use semantic path hash as seed for deterministic shuffle
    std::uint32_t seed = compute_state_id();

    // Fisher-Yates shuffle with simple LCG
    std::vector<std::uint16_t> result = buffers;
    for (std::size_t i = result.size() - 1; i > 0; --i) {
        seed = seed * 1103515245 + 12345;  // Simple LCG
        std::size_t j = (seed >> 16) % (i + 1);
        std::swap(result[i], result[j]);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result),
                           node_buffers_, multi_buffers_);
}

// sort(array) - sort elements in ascending order (compile-time constants only)
std::uint16_t CodeGenerator::handle_sort_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E169", "sort() requires 1 argument: sort(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // For compile-time sort, we need to extract literal values
    // This only works for array literals with number elements
    const Node& arr_node = ast_->arena[args.nodes[0]];
    if (arr_node.type != NodeType::ArrayLit) {
        // For non-literals, just pass through (can't sort at compile time)
        std::uint16_t array_buf = visit(args.nodes[0]);
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    // Extract literal values
    std::vector<std::pair<float, NodeIndex>> values;
    NodeIndex elem = arr_node.first_child;
    while (elem != NULL_NODE) {
        const Node& elem_node = ast_->arena[elem];
        if (elem_node.type == NodeType::NumberLit) {
            values.push_back({static_cast<float>(elem_node.as_number()), elem});
        } else {
            // Non-literal element - can't sort at compile time
            std::uint16_t array_buf = visit(args.nodes[0]);
            node_buffers_[node] = array_buf;
            return array_buf;
        }
        elem = ast_->arena[elem].next_sibling;
    }

    if (values.empty()) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    // Sort by value
    std::sort(values.begin(), values.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Emit sorted constants
    std::vector<std::uint16_t> result_buffers;
    for (const auto& [val, _] : values) {
        std::uint16_t buf = emit_push_const(buffers_, instructions_, val);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        result_buffers.push_back(buf);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// normalize(array) - scale to 0-1 range
std::uint16_t CodeGenerator::handle_normalize_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E170", "normalize() requires 1 argument: normalize(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element normalizes to 0 (or could be 1, convention varies)
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.size() <= 1) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    // Find min
    std::uint16_t min_buf = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        min_buf = emit_binary_op(buffers_, instructions_, cedar::Opcode::MIN, min_buf, buffers[i]);
    }

    // Find max
    std::uint16_t max_buf = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        max_buf = emit_binary_op(buffers_, instructions_, cedar::Opcode::MAX, max_buf, buffers[i]);
    }

    // range = max - min
    std::uint16_t range_buf = emit_binary_op(buffers_, instructions_, cedar::Opcode::SUB, max_buf, min_buf);

    // Normalize each element: (elem - min) / range
    std::vector<std::uint16_t> result_buffers;
    for (auto buf : buffers) {
        std::uint16_t shifted = emit_binary_op(buffers_, instructions_, cedar::Opcode::SUB, buf, min_buf);
        std::uint16_t normalized = emit_binary_op(buffers_, instructions_, cedar::Opcode::DIV, shifted, range_buf);
        result_buffers.push_back(normalized);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// scale(array, lo, hi) - map to new range
std::uint16_t CodeGenerator::handle_scale_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E171", "scale() requires 3 arguments: scale(array, lo, hi)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);
    std::uint16_t lo_buf = visit(args.nodes[1]);
    std::uint16_t hi_buf = visit(args.nodes[2]);

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - just return lo (or could interpolate)
        node_buffers_[node] = lo_buf;
        return lo_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.size() <= 1) {
        node_buffers_[node] = lo_buf;
        return lo_buf;
    }

    // First normalize to 0-1
    // Find min
    std::uint16_t min_buf = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        min_buf = emit_binary_op(buffers_, instructions_, cedar::Opcode::MIN, min_buf, buffers[i]);
    }

    // Find max
    std::uint16_t max_buf = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        max_buf = emit_binary_op(buffers_, instructions_, cedar::Opcode::MAX, max_buf, buffers[i]);
    }

    // range = max - min
    std::uint16_t src_range = emit_binary_op(buffers_, instructions_, cedar::Opcode::SUB, max_buf, min_buf);
    // target_range = hi - lo
    std::uint16_t dst_range = emit_binary_op(buffers_, instructions_, cedar::Opcode::SUB, hi_buf, lo_buf);

    // Scale each element: ((elem - min) / src_range) * dst_range + lo
    std::vector<std::uint16_t> result_buffers;
    for (auto buf : buffers) {
        std::uint16_t shifted = emit_binary_op(buffers_, instructions_, cedar::Opcode::SUB, buf, min_buf);
        std::uint16_t normalized = emit_binary_op(buffers_, instructions_, cedar::Opcode::DIV, shifted, src_range);
        std::uint16_t scaled = emit_binary_op(buffers_, instructions_, cedar::Opcode::MUL, normalized, dst_range);
        std::uint16_t result = emit_binary_op(buffers_, instructions_, cedar::Opcode::ADD, scaled, lo_buf);
        result_buffers.push_back(result);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// ============================================================================
// Array generation operations
// ============================================================================

// linspace(start, end, n) - N evenly spaced values
std::uint16_t CodeGenerator::handle_linspace_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E172", "linspace() requires 3 arguments: linspace(start, end, n)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // All arguments must be compile-time constants
    auto start_val = resolve_const_scalar(ast_->arena, args.nodes[0], *symbols_);
    auto end_val = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);
    auto n_val = resolve_const_scalar(ast_->arena, args.nodes[2], *symbols_);

    if (!start_val || !end_val || !n_val) {
        error("E173", "linspace() arguments must be compile-time constants", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    float start = static_cast<float>(*start_val);
    float end = static_cast<float>(*end_val);
    int count = static_cast<int>(*n_val);

    if (count <= 0) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    std::vector<std::uint16_t> result_buffers;
    for (int i = 0; i < count; ++i) {
        float t = (count > 1) ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        float val = start + t * (end - start);
        std::uint16_t buf = emit_push_const(buffers_, instructions_, val);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        result_buffers.push_back(buf);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// random(n) - N random values (deterministic by path)
std::uint16_t CodeGenerator::handle_random_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E174", "random() requires 1 argument: random(n)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto n_val = resolve_const_scalar(ast_->arena, args.nodes[0], *symbols_);
    if (!n_val) {
        error("E175", "random() argument must be a compile-time constant",
              ast_->arena[args.nodes[0]].location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    int count = static_cast<int>(*n_val);
    if (count <= 0) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    // Use semantic path hash as seed for deterministic random values
    std::uint32_t seed = compute_state_id();

    std::vector<std::uint16_t> result_buffers;
    for (int i = 0; i < count; ++i) {
        // Simple LCG to generate random float in [0, 1)
        seed = seed * 1103515245 + 12345;
        float val = static_cast<float>((seed >> 16) & 0x7FFF) / 32768.0f;

        std::uint16_t buf = emit_push_const(buffers_, instructions_, val);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        result_buffers.push_back(buf);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// harmonics(fundamental, n) - harmonic series
std::uint16_t CodeGenerator::handle_harmonics_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E176", "harmonics() requires 2 arguments: harmonics(fundamental, n)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto fund_val = resolve_const_scalar(ast_->arena, args.nodes[0], *symbols_);
    auto n_val = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);

    if (!fund_val || !n_val) {
        error("E177", "harmonics() arguments must be compile-time constants", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    float fundamental = static_cast<float>(*fund_val);
    int count = static_cast<int>(*n_val);

    if (count <= 0) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    std::vector<std::uint16_t> result_buffers;
    for (int i = 1; i <= count; ++i) {
        float harmonic = fundamental * static_cast<float>(i);
        std::uint16_t buf = emit_push_const(buffers_, instructions_, harmonic);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        result_buffers.push_back(buf);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

} // namespace akkado

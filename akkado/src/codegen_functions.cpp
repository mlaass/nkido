// User-defined function and match expression codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include <cstring>

namespace akkado {

using codegen::encode_const_value;

// User function call handler - inlines function bodies at call sites
std::uint16_t CodeGenerator::handle_user_function_call(
    NodeIndex node, const Node& n, const UserFunctionInfo& func) {

    // Collect call arguments
    std::vector<NodeIndex> args;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = ast_->arena[arg];
        NodeIndex arg_value = arg;
        if (arg_node.type == NodeType::Argument) {
            arg_value = arg_node.first_child;
        }
        args.push_back(arg_value);
        arg = ast_->arena[arg].next_sibling;
    }

    // Save param_literals, param_string_defaults, param_multi_buffer_sources, and param_function_refs for this scope
    auto saved_param_literals = std::move(param_literals_);
    auto saved_param_string_defaults = std::move(param_string_defaults_);
    auto saved_param_multi_buffer_sources = std::move(param_multi_buffer_sources_);
    auto saved_param_function_refs = std::move(param_function_refs_);
    param_literals_.clear();
    param_string_defaults_.clear();
    param_multi_buffer_sources_.clear();
    param_function_refs_.clear();

    // IMPORTANT: Visit arguments BEFORE pushing scope to evaluate them in caller's context
    // This allows nested function calls like double(double(x)) to work correctly.
    std::vector<std::uint16_t> param_bufs;
    // Track rest param info separately
    std::string rest_param_name;
    std::vector<std::uint16_t> rest_buffers;

    for (std::size_t i = 0; i < func.params.size(); ++i) {
        std::uint16_t param_buf;

        if (func.params[i].is_rest) {
            // Rest parameter: collect all remaining arguments
            rest_param_name = func.params[i].name;
            for (std::size_t j = i; j < args.size(); ++j) {
                std::uint16_t buf = visit(args[j]);
                rest_buffers.push_back(buf);
            }
            param_buf = rest_buffers.empty() ? BufferAllocator::BUFFER_UNUSED : rest_buffers[0];
            param_bufs.push_back(param_buf);
            break;  // Rest must be last param
        } else if (i < args.size()) {
            // Check if the argument is a closure or function reference
            auto func_ref = resolve_function_arg(args[i]);
            if (func_ref) {
                // Store as function ref — don't visit (would eagerly compile with unbound params)
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                param_function_refs_[param_hash] = *func_ref;
                param_buf = BufferAllocator::BUFFER_UNUSED;  // placeholder — never used as audio
            } else {
                // Check if the argument is a literal - record for match resolution
                const Node& arg_node = ast_->arena[args[i]];
                if (arg_node.type == NodeType::StringLit ||
                    arg_node.type == NodeType::NumberLit ||
                    arg_node.type == NodeType::BoolLit) {
                    std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                    param_literals_[param_hash] = args[i];
                }

                // Visit argument in caller's scope
                param_buf = visit(args[i]);

                // Track multi-buffer arguments for polyphonic propagation
                if (is_multi_buffer(args[i])) {
                    std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                    param_multi_buffer_sources_[param_hash] = args[i];
                }
            }
        } else if (func.params[i].default_value.has_value()) {
            // Use numeric default value
            param_buf = buffers_.allocate();
            if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                param_literals_ = std::move(saved_param_literals);
                return BufferAllocator::BUFFER_UNUSED;
            }

            cedar::Instruction push_inst{};
            push_inst.opcode = cedar::Opcode::PUSH_CONST;
            push_inst.out_buffer = param_buf;
            push_inst.inputs[0] = 0xFFFF;
            push_inst.inputs[1] = 0xFFFF;
            push_inst.inputs[2] = 0xFFFF;
            push_inst.inputs[3] = 0xFFFF;

            float default_val = static_cast<float>(*func.params[i].default_value);
            encode_const_value(push_inst, default_val);
            emit(push_inst);
        } else if (func.params[i].default_string.has_value()) {
            // String default: doesn't produce audio — used for compile-time match dispatch
            param_buf = BufferAllocator::BUFFER_UNUSED;
            std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
            param_string_defaults_[param_hash] = *func.params[i].default_string;
        } else {
            // Missing required argument - should have been caught by analyzer
            error("E105", "Missing required argument for parameter '" +
                  func.params[i].name + "'", n.location);
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            return BufferAllocator::BUFFER_UNUSED;
        }

        param_bufs.push_back(param_buf);
    }

    // NOW push scope for function parameters and bind them
    symbols_->push_scope();
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        if (func.params[i].is_rest) {
            // Bind rest param as synthetic Array
            ArrayInfo arr;
            arr.source_node = NULL_NODE;
            arr.buffer_indices = rest_buffers;
            arr.element_count = rest_buffers.size();
            symbols_->define_array(func.params[i].name, arr);
        } else {
            std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
            auto fref_it = param_function_refs_.find(param_hash);
            if (fref_it != param_function_refs_.end()) {
                symbols_->define_function_value(func.params[i].name, fref_it->second);
            } else {
                symbols_->define_variable(func.params[i].name, param_bufs[i]);
            }
        }
    }

    // Check if function was explicitly defined to return a closure
    // (e.g. fn make_filter(cut) -> (sig) -> lp(sig, cut))
    // Only triggers for source-level closures, not pipe-rewritten ones
    if (func.returns_closure && func.body_node != NULL_NODE &&
        ast_->arena[func.body_node].type == NodeType::Closure) {
        // Build FunctionRef from the closure body
        auto ref = resolve_function_arg(func.body_node);
        if (ref) {
            // Add captures for bound function parameters
            for (std::size_t i = 0; i < func.params.size(); ++i) {
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                auto fref_it = param_function_refs_.find(param_hash);
                if (fref_it != param_function_refs_.end()) {
                    // Param is itself a function ref — add its captures transitively
                    // (the closure body can reference the outer fn's function params)
                    // For now, skip — function params are resolved via param_function_refs_
                } else if (param_bufs[i] != BufferAllocator::BUFFER_UNUSED) {
                    ref->captures.push_back({func.params[i].name, param_bufs[i]});
                }
                // Also propagate string defaults as captures won't help for strings,
                // but the param_string_defaults_ will be set when the closure is later called
            }
            pending_function_ref_ = *ref;
        }

        // Pop scope and restore state
        symbols_->pop_scope();
        param_literals_ = std::move(saved_param_literals);
        param_string_defaults_ = std::move(saved_param_string_defaults);
        param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
        param_function_refs_ = std::move(saved_param_function_refs);

        node_buffers_[node] = BufferAllocator::BUFFER_UNUSED;
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Save node_buffers_ state before visiting body.
    // This is necessary because function bodies are shared AST nodes that may be
    // visited multiple times with different parameter bindings.
    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    // Visit function body (inline expansion)
    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;
    if (func.body_node != NULL_NODE) {
        result = visit(func.body_node);
    }

    // Restore node_buffers_ (keep new entries but restore old ones)
    for (auto& [k, v] : saved_node_buffers) {
        if (node_buffers_.find(k) == node_buffers_.end()) {
            node_buffers_[k] = v;
        }
    }

    // Pop scope and restore param_literals, param_string_defaults, param_multi_buffer_sources, and param_function_refs
    symbols_->pop_scope();
    param_literals_ = std::move(saved_param_literals);
    param_string_defaults_ = std::move(saved_param_string_defaults);
    param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
    param_function_refs_ = std::move(saved_param_function_refs);

    node_buffers_[node] = result;
    return result;
}

// FunctionValue (lambda variable) call handler - inlines closure bodies at call sites
std::uint16_t CodeGenerator::handle_function_value_call(
    NodeIndex node, const Node& n, const FunctionRef& func) {

    // Collect call arguments
    std::vector<NodeIndex> args;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = ast_->arena[arg];
        NodeIndex arg_value = arg;
        if (arg_node.type == NodeType::Argument) {
            arg_value = arg_node.first_child;
        }
        args.push_back(arg_value);
        arg = ast_->arena[arg].next_sibling;
    }

    // Save param_literals and param_string_defaults for this scope
    auto saved_param_literals = std::move(param_literals_);
    auto saved_param_string_defaults = std::move(param_string_defaults_);
    param_literals_.clear();
    param_string_defaults_.clear();

    // Visit arguments BEFORE pushing scope to evaluate them in caller's context
    std::vector<std::uint16_t> param_bufs;
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        std::uint16_t param_buf;

        if (i < args.size()) {
            // Check if the argument is a literal - record for match resolution
            const Node& arg_node = ast_->arena[args[i]];
            if (arg_node.type == NodeType::StringLit ||
                arg_node.type == NodeType::NumberLit ||
                arg_node.type == NodeType::BoolLit) {
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                param_literals_[param_hash] = args[i];
            }

            // Visit argument in caller's scope
            param_buf = visit(args[i]);
        } else if (func.params[i].default_value.has_value()) {
            // Use numeric default value
            param_buf = buffers_.allocate();
            if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                return BufferAllocator::BUFFER_UNUSED;
            }

            cedar::Instruction push_inst{};
            push_inst.opcode = cedar::Opcode::PUSH_CONST;
            push_inst.out_buffer = param_buf;
            push_inst.inputs[0] = 0xFFFF;
            push_inst.inputs[1] = 0xFFFF;
            push_inst.inputs[2] = 0xFFFF;
            push_inst.inputs[3] = 0xFFFF;

            float default_val = static_cast<float>(*func.params[i].default_value);
            encode_const_value(push_inst, default_val);
            emit(push_inst);
        } else if (func.params[i].default_string.has_value()) {
            // String default: doesn't produce audio — used for compile-time match dispatch
            param_buf = BufferAllocator::BUFFER_UNUSED;
            std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
            param_string_defaults_[param_hash] = *func.params[i].default_string;
        } else {
            // Missing required argument - should have been caught by analyzer
            error("E105", "Missing required argument for parameter '" +
                  func.params[i].name + "'", n.location);
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            return BufferAllocator::BUFFER_UNUSED;
        }

        param_bufs.push_back(param_buf);
    }

    // Push scope for captures and parameters
    symbols_->push_scope();
    for (const auto& capture : func.captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        symbols_->define_variable(func.params[i].name, param_bufs[i]);
    }

    // Push semantic path for unique state IDs
    const std::string& callee_name = n.as_identifier();
    std::uint32_t count = call_counters_[callee_name]++;
    push_path(callee_name + "#" + std::to_string(count));

    // Save node_buffers_ state before visiting body
    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    // Handle composed functions: apply each function in the chain sequentially
    if (!func.compose_chain.empty()) {
        // First function gets the call argument(s)
        if (!param_bufs.empty()) {
            result = param_bufs[0];
        }
        for (const auto& chain_ref : func.compose_chain) {
            result = apply_function_ref(chain_ref, result, n.location);
        }
    } else {
        // Normal function value call
        // Find the body node
        NodeIndex body = NULL_NODE;
        if (func.is_user_function) {
            body = func.closure_node;
        } else {
            const Node& closure_node = ast_->arena[func.closure_node];
            NodeIndex child = closure_node.first_child;
            while (child != NULL_NODE) {
                const Node& child_node = ast_->arena[child];
                if (child_node.type == NodeType::Identifier) {
                    // parameter — skip
                } else {
                    body = child;
                    break;
                }
                child = ast_->arena[child].next_sibling;
            }
        }

        // Visit closure body (inline expansion)
        if (body != NULL_NODE) {
            result = visit(body);
        }
    }

    // Restore node_buffers_
    for (auto& [k, v] : saved_node_buffers) {
        if (node_buffers_.find(k) == node_buffers_.end()) {
            node_buffers_[k] = v;
        }
    }

    // Pop semantic path, scope, and restore param_literals and param_string_defaults
    pop_path();
    symbols_->pop_scope();
    param_literals_ = std::move(saved_param_literals);
    param_string_defaults_ = std::move(saved_param_string_defaults);

    node_buffers_[node] = result;
    return result;
}

// Handle compose(f, g, ...) - create a composed function reference
std::uint16_t CodeGenerator::handle_compose_call(NodeIndex node, const Node& n) {
    // Collect function arguments
    std::vector<FunctionRef> refs;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = ast_->arena[arg];
        NodeIndex arg_value = arg;
        if (arg_node.type == NodeType::Argument) {
            arg_value = arg_node.first_child;
        }
        auto ref = resolve_function_arg(arg_value);
        if (!ref) {
            error("E140", "compose() arguments must be functions or closures", arg_node.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        refs.push_back(*ref);
        arg = ast_->arena[arg].next_sibling;
    }

    if (refs.size() < 2) {
        error("E141", "compose() requires at least 2 function arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Build composed FunctionRef: first function's params, chain of all refs
    FunctionRef composed{};
    composed.params = refs[0].params;  // Same params as first function
    composed.closure_node = refs[0].closure_node;
    composed.is_user_function = false;
    composed.captures = refs[0].captures;
    composed.compose_chain = refs;  // Store entire chain

    pending_function_ref_ = composed;
    node_buffers_[node] = BufferAllocator::BUFFER_UNUSED;
    return BufferAllocator::BUFFER_UNUSED;
}

// Handle Closure nodes - allocate buffers for parameters and generate body
std::uint16_t CodeGenerator::handle_closure(NodeIndex node, const Node& n) {
    // For simple closures: allocate buffers for parameters, then generate body
    // Find parameters and body
    // Parameters may be stored as IdentifierData or ClosureParamData
    std::vector<std::string> param_names;
    NodeIndex child = n.first_child;
    NodeIndex body = NULL_NODE;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            // Check if it's IdentifierData or ClosureParamData
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
        error("E112", "Closure has no body", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Allocate input buffers for parameters and bind them
    for (const auto& param : param_names) {
        std::uint16_t param_buf = buffers_.allocate();
        if (param_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        // Update symbol table with actual buffer index
        symbols_->define_variable(param, param_buf);
    }

    // Generate code for body
    std::uint16_t body_buf = visit(body);
    node_buffers_[node] = body_buf;
    return body_buf;
}

// Check if a match expression can be resolved at compile time
bool CodeGenerator::is_compile_time_match(NodeIndex node, const Node& n) const {
    // Get match expression data
    if (!std::holds_alternative<Node::MatchExprData>(n.data)) {
        return false;
    }
    const auto& match_data = n.as_match_expr();

    // For guard-only form without scrutinee, we need runtime evaluation
    // unless all guards are compile-time constants
    if (!match_data.has_scrutinee) {
        // Guard-only: check if all guards are compile-time evaluable
        NodeIndex arm = n.first_child;
        while (arm != NULL_NODE) {
            const Node& arm_node = ast_->arena[arm];
            if (arm_node.type == NodeType::MatchArm) {
                const auto& arm_data = arm_node.as_match_arm();
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    const Node& guard_node = ast_->arena[arm_data.guard_node];
                    // Only simple literals are compile-time evaluable
                    if (guard_node.type != NodeType::BoolLit &&
                        guard_node.type != NodeType::NumberLit) {
                        return false;  // Non-const guard -> runtime
                    }
                }
            }
            arm = ast_->arena[arm].next_sibling;
        }
        return true;
    }

    // Scrutinee form: check if scrutinee resolves to a literal
    NodeIndex scrutinee = n.first_child;
    if (scrutinee == NULL_NODE) return false;

    const Node* scrutinee_ptr = &ast_->arena[scrutinee];

    // If scrutinee is an Identifier, check if it maps to a literal argument or string default
    if (scrutinee_ptr->type == NodeType::Identifier) {
        std::string param_name;
        if (std::holds_alternative<Node::ClosureParamData>(scrutinee_ptr->data)) {
            param_name = scrutinee_ptr->as_closure_param().name;
        } else if (std::holds_alternative<Node::IdentifierData>(scrutinee_ptr->data)) {
            param_name = scrutinee_ptr->as_identifier();
        }

        if (!param_name.empty()) {
            std::uint32_t param_hash = fnv1a_hash(param_name);
            auto it = param_literals_.find(param_hash);
            if (it != param_literals_.end()) {
                scrutinee_ptr = &ast_->arena[it->second];
            } else if (param_string_defaults_.count(param_hash)) {
                // String default found — compile-time match is possible
                // (handled in handle_compile_time_match via param_string_defaults_)
                scrutinee_ptr = nullptr;  // Signal: use string default path below
            } else {
                return false;  // Variable without known literal -> runtime
            }
        }
    }

    // Check if scrutinee is a literal (or string default)
    if (scrutinee_ptr != nullptr &&
        scrutinee_ptr->type != NodeType::StringLit &&
        scrutinee_ptr->type != NodeType::NumberLit &&
        scrutinee_ptr->type != NodeType::BoolLit) {
        return false;  // Non-literal scrutinee -> runtime
    }

    // Check all guards for const-evaluability
    NodeIndex arm = ast_->arena[scrutinee].next_sibling;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();
            if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                const Node& guard_node = ast_->arena[arm_data.guard_node];
                if (guard_node.type != NodeType::BoolLit &&
                    guard_node.type != NodeType::NumberLit) {
                    return false;  // Non-const guard -> runtime
                }
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    return true;
}

// Handle compile-time match - evaluate patterns and guards, emit only winning branch
std::uint16_t CodeGenerator::handle_compile_time_match(NodeIndex node, const Node& n) {
    const auto& match_data = n.as_match_expr();

    NodeIndex first_arm = n.first_child;
    const Node* scrutinee_ptr = nullptr;
    std::string scrutinee_key;

    if (match_data.has_scrutinee) {
        NodeIndex scrutinee = n.first_child;
        first_arm = ast_->arena[scrutinee].next_sibling;
        scrutinee_ptr = &ast_->arena[scrutinee];

        // If scrutinee is an Identifier, check if it maps to a literal argument or string default
        if (scrutinee_ptr->type == NodeType::Identifier) {
            std::string param_name;
            if (std::holds_alternative<Node::ClosureParamData>(scrutinee_ptr->data)) {
                param_name = scrutinee_ptr->as_closure_param().name;
            } else if (std::holds_alternative<Node::IdentifierData>(scrutinee_ptr->data)) {
                param_name = scrutinee_ptr->as_identifier();
            }

            if (!param_name.empty()) {
                std::uint32_t param_hash = fnv1a_hash(param_name);
                auto it = param_literals_.find(param_hash);
                if (it != param_literals_.end()) {
                    scrutinee_ptr = &ast_->arena[it->second];
                } else {
                    auto str_it = param_string_defaults_.find(param_hash);
                    if (str_it != param_string_defaults_.end()) {
                        // String default — set key directly, no AST node needed
                        scrutinee_key = "s:" + str_it->second;
                        scrutinee_ptr = nullptr;  // Skip node-based key extraction below
                    }
                }
            }
        }

        // Get scrutinee value for matching (skip if already set from string default)
        if (scrutinee_ptr != nullptr) {
            if (scrutinee_ptr->type == NodeType::StringLit) {
                scrutinee_key = "s:" + scrutinee_ptr->as_string();
            } else if (scrutinee_ptr->type == NodeType::NumberLit) {
                scrutinee_key = "n:" + std::to_string(scrutinee_ptr->as_number());
            } else if (scrutinee_ptr->type == NodeType::BoolLit) {
                scrutinee_key = "b:" + std::to_string(scrutinee_ptr->as_bool());
            }
        }
    }

    // Find matching arm
    NodeIndex arm = first_arm;
    NodeIndex default_body = NULL_NODE;

    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();

            // Get pattern (first child) and body (second child)
            NodeIndex pattern = arm_node.first_child;
            NodeIndex body = (pattern != NULL_NODE) ?
                            ast_->arena[pattern].next_sibling : NULL_NODE;

            if (arm_data.is_wildcard) {
                default_body = body;
            } else if (match_data.has_scrutinee) {
                // Scrutinee form: check pattern match
                if (pattern != NULL_NODE) {
                    const Node& pattern_node = ast_->arena[pattern];
                    std::string pattern_key;

                    if (pattern_node.type == NodeType::StringLit) {
                        pattern_key = "s:" + pattern_node.as_string();
                    } else if (pattern_node.type == NodeType::NumberLit) {
                        pattern_key = "n:" + std::to_string(pattern_node.as_number());
                    } else if (pattern_node.type == NodeType::BoolLit) {
                        pattern_key = "b:" + std::to_string(pattern_node.as_bool());
                    }

                    if (pattern_key == scrutinee_key) {
                        // Pattern matches - check guard if present
                        bool guard_passes = true;
                        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                            const Node& guard_node = ast_->arena[arm_data.guard_node];
                            if (guard_node.type == NodeType::BoolLit) {
                                guard_passes = guard_node.as_bool();
                            } else if (guard_node.type == NodeType::NumberLit) {
                                guard_passes = guard_node.as_number() != 0.0;
                            }
                        }

                        if (guard_passes && body != NULL_NODE) {
                            std::uint16_t result = visit(body);
                            node_buffers_[node] = result;
                            return result;
                        }
                    }
                }
            } else {
                // Guard-only form: evaluate guard
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    const Node& guard_node = ast_->arena[arm_data.guard_node];
                    bool guard_passes = false;
                    if (guard_node.type == NodeType::BoolLit) {
                        guard_passes = guard_node.as_bool();
                    } else if (guard_node.type == NodeType::NumberLit) {
                        guard_passes = guard_node.as_number() != 0.0;
                    }

                    if (guard_passes && body != NULL_NODE) {
                        std::uint16_t result = visit(body);
                        node_buffers_[node] = result;
                        return result;
                    }
                }
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    // No match found - use default if available
    if (default_body != NULL_NODE) {
        std::uint16_t result = visit(default_body);
        node_buffers_[node] = result;
        return result;
    }

    error("E121", "No matching pattern in match expression", n.location);
    return BufferAllocator::BUFFER_UNUSED;
}

// Handle runtime match - emit all branches and build nested select chain
std::uint16_t CodeGenerator::handle_runtime_match(NodeIndex node, const Node& n) {
    const auto& match_data = n.as_match_expr();

    // Check for missing wildcard arm and warn
    bool has_wildcard = false;
    NodeIndex first_arm = n.first_child;

    if (match_data.has_scrutinee) {
        first_arm = ast_->arena[first_arm].next_sibling;
    }

    NodeIndex arm = first_arm;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();
            if (arm_data.is_wildcard) {
                has_wildcard = true;
                break;
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    if (!has_wildcard) {
        warn("W001", "Match expression missing default '_' arm; defaulting to 0.0", n.location);
    }

    // Visit scrutinee if present
    std::uint16_t scrutinee_buf = BufferAllocator::BUFFER_UNUSED;
    if (match_data.has_scrutinee) {
        scrutinee_buf = visit(n.first_child);
    }

    // Collect all arms: condition buffer, body buffer, is_wildcard
    struct ArmInfo {
        std::uint16_t cond_buf;
        std::uint16_t body_buf;
        bool is_wildcard;
    };
    std::vector<ArmInfo> arms;

    arm = first_arm;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();

            NodeIndex pattern = arm_node.first_child;
            NodeIndex body = (pattern != NULL_NODE) ?
                            ast_->arena[pattern].next_sibling : NULL_NODE;

            // Visit body first (all branches compute in DSP)
            std::uint16_t body_buf = BufferAllocator::BUFFER_UNUSED;
            if (body != NULL_NODE) {
                body_buf = visit(body);
            } else {
                // Empty body -> emit 0.0
                body_buf = buffers_.allocate();
                cedar::Instruction push_inst{};
                push_inst.opcode = cedar::Opcode::PUSH_CONST;
                push_inst.out_buffer = body_buf;
                push_inst.inputs[0] = 0xFFFF;
                push_inst.inputs[1] = 0xFFFF;
                push_inst.inputs[2] = 0xFFFF;
                push_inst.inputs[3] = 0xFFFF;
                codegen::encode_const_value(push_inst, 0.0f);
                emit(push_inst);
            }

            if (arm_data.is_wildcard) {
                arms.push_back({BufferAllocator::BUFFER_UNUSED, body_buf, true});
            } else {
                // Build condition
                std::uint16_t cond_buf = BufferAllocator::BUFFER_UNUSED;

                if (match_data.has_scrutinee) {
                    // Scrutinee form: eq(scrutinee, pattern)
                    std::uint16_t pattern_buf = visit(pattern);

                    cond_buf = buffers_.allocate();
                    cedar::Instruction eq_inst{};
                    eq_inst.opcode = cedar::Opcode::CMP_EQ;
                    eq_inst.out_buffer = cond_buf;
                    eq_inst.inputs[0] = scrutinee_buf;
                    eq_inst.inputs[1] = pattern_buf;
                    eq_inst.inputs[2] = 0xFFFF;
                    eq_inst.inputs[3] = 0xFFFF;
                    emit(eq_inst);
                } else {
                    // Guard-only form: condition is the guard itself
                    if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                        cond_buf = visit(arm_data.guard_node);
                    }
                }

                // If there's a guard, AND it with the pattern condition
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE && match_data.has_scrutinee) {
                    std::uint16_t guard_buf = visit(arm_data.guard_node);

                    std::uint16_t combined_buf = buffers_.allocate();
                    cedar::Instruction and_inst{};
                    and_inst.opcode = cedar::Opcode::LOGIC_AND;
                    and_inst.out_buffer = combined_buf;
                    and_inst.inputs[0] = cond_buf;
                    and_inst.inputs[1] = guard_buf;
                    and_inst.inputs[2] = 0xFFFF;
                    and_inst.inputs[3] = 0xFFFF;
                    emit(and_inst);
                    cond_buf = combined_buf;
                }

                arms.push_back({cond_buf, body_buf, false});
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    if (arms.empty()) {
        error("E122", "Match expression has no arms", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Build nested select chain (reverse order)
    // Start with default (wildcard arm or 0.0)
    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    // Find wildcard arm for default
    for (const auto& arm_info : arms) {
        if (arm_info.is_wildcard) {
            result = arm_info.body_buf;
            break;
        }
    }

    // If no wildcard, emit 0.0 as default
    if (result == BufferAllocator::BUFFER_UNUSED) {
        result = buffers_.allocate();
        cedar::Instruction push_inst{};
        push_inst.opcode = cedar::Opcode::PUSH_CONST;
        push_inst.out_buffer = result;
        push_inst.inputs[0] = 0xFFFF;
        push_inst.inputs[1] = 0xFFFF;
        push_inst.inputs[2] = 0xFFFF;
        push_inst.inputs[3] = 0xFFFF;
        codegen::encode_const_value(push_inst, 0.0f);
        emit(push_inst);
    }

    // Build select chain in reverse order (last non-wildcard arm first)
    for (auto it = arms.rbegin(); it != arms.rend(); ++it) {
        if (!it->is_wildcard && it->cond_buf != BufferAllocator::BUFFER_UNUSED) {
            std::uint16_t select_buf = buffers_.allocate();
            cedar::Instruction sel_inst{};
            sel_inst.opcode = cedar::Opcode::SELECT;
            sel_inst.out_buffer = select_buf;
            sel_inst.inputs[0] = it->cond_buf;   // condition
            sel_inst.inputs[1] = it->body_buf;   // true branch
            sel_inst.inputs[2] = result;         // false branch (previous result)
            sel_inst.inputs[3] = 0xFFFF;
            emit(sel_inst);
            result = select_buf;
        }
    }

    node_buffers_[node] = result;
    return result;
}

// Handle MatchExpr nodes - dispatch to compile-time or runtime handling
std::uint16_t CodeGenerator::handle_match_expr(NodeIndex node, const Node& n) {
    // Check if match expression data exists
    if (!std::holds_alternative<Node::MatchExprData>(n.data)) {
        // Legacy: no MatchExprData, assume scrutinee form without guards
        // Fall back to compile-time behavior for backwards compatibility
        return handle_compile_time_match(node, n);
    }

    if (is_compile_time_match(node, n)) {
        return handle_compile_time_match(node, n);
    } else {
        return handle_runtime_match(node, n);
    }
}

// Handle tap_delay(in, time, fb, processor) - tap delay with inline feedback chain
// Also handles tap_delay_ms and tap_delay_smp variants with different time units.
// Emits DELAY_TAP, compiles processor closure inline, then emits DELAY_WRITE
// Both opcodes share the same state_id to operate on the same delay buffer
std::uint16_t CodeGenerator::handle_tap_delay_call(NodeIndex node, const Node& n) {
    // Extract function name to determine time unit
    const std::string& func_name = n.as_identifier();

    // Determine time unit from function name
    // 0 = seconds (default), 1 = milliseconds, 2 = samples
    std::uint8_t time_unit = 0;
    if (func_name == "tap_delay_ms") {
        time_unit = 1;
    } else if (func_name == "tap_delay_smp") {
        time_unit = 2;
    }

    // Collect arguments: in, time, fb, processor
    std::vector<NodeIndex> args;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = ast_->arena[arg];
        NodeIndex arg_value = arg;
        if (arg_node.type == NodeType::Argument) {
            arg_value = arg_node.first_child;
        }
        args.push_back(arg_value);
        arg = ast_->arena[arg].next_sibling;
    }

    if (args.size() < 4) {
        error("E301", func_name + "() requires 4 arguments: " + func_name + "(in, time, fb, processor)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Validate that processor (4th arg) is a Closure
    const Node& processor_node = ast_->arena[args[3]];
    if (processor_node.type != NodeType::Closure) {
        error("E302", "tap_delay() 4th argument must be a closure: (x) -> ...", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Extract closure parameter (must have exactly 1 parameter)
    std::string param_name;
    NodeIndex closure_body = NULL_NODE;
    NodeIndex child = processor_node.first_child;
    int param_count = 0;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_name = child_node.as_closure_param().name;
                param_count++;
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_name = child_node.as_identifier();
                param_count++;
            } else {
                // Not a parameter - this is the body
                closure_body = child;
                break;
            }
        } else {
            // Body node
            closure_body = child;
            break;
        }
        child = ast_->arena[child].next_sibling;
    }

    if (param_count != 1) {
        error("E303", "tap_delay() processor closure must have exactly 1 parameter", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (closure_body == NULL_NODE) {
        error("E304", "tap_delay() processor closure has no body", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Push path for semantic ID tracking (before TAP)
    std::uint32_t count = call_counters_["tap_delay"]++;
    std::string unique_name = "tap_delay#" + std::to_string(count);
    push_path(unique_name);

    // Compute shared state_id for TAP and WRITE
    std::uint32_t delay_state_id = compute_state_id();

    // Visit input arguments (in, time, fb)
    std::uint16_t in_buf = visit(args[0]);
    std::uint16_t time_buf = visit(args[1]);
    std::uint16_t fb_buf = visit(args[2]);

    // Handle optional dry/wet arguments (args[4], args[5])
    // Default: dry=0.0, wet=1.0 (100% wet, backward compatible)
    std::uint16_t dry_buf = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t wet_buf = BufferAllocator::BUFFER_UNUSED;

    if (args.size() > 4) {
        dry_buf = visit(args[4]);
    }
    if (args.size() > 5) {
        wet_buf = visit(args[5]);
    }

    // Allocate output buffer for DELAY_TAP
    std::uint16_t tap_out_buf = buffers_.allocate();
    if (tap_out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit DELAY_TAP instruction
    // DELAY_TAP: reads from delay buffer, outputs delayed signal
    // in0: input (passed through for signal flow), in1: delay time
    // rate field: time unit (0=seconds, 1=ms, 2=samples)
    cedar::Instruction tap_inst{};
    tap_inst.opcode = cedar::Opcode::DELAY_TAP;
    tap_inst.out_buffer = tap_out_buf;
    tap_inst.inputs[0] = in_buf;
    tap_inst.inputs[1] = time_buf;
    tap_inst.inputs[2] = 0xFFFF;
    tap_inst.inputs[3] = 0xFFFF;
    tap_inst.inputs[4] = 0xFFFF;
    tap_inst.rate = time_unit;
    tap_inst.state_id = delay_state_id;
    emit(tap_inst);

    // Push scope and bind closure parameter to TAP output
    symbols_->push_scope();
    symbols_->define_variable(param_name, tap_out_buf);

    // Push semantic context for nested opcodes
    push_path("fb");

    // Save node_buffers_ state before visiting closure body
    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    // Compile the closure body (feedback chain)
    std::uint16_t processed_buf = visit(closure_body);

    // Restore node_buffers_
    node_buffers_ = std::move(saved_node_buffers);

    // Pop semantic context
    pop_path();

    // Pop scope
    symbols_->pop_scope();

    // Allocate output buffer for DELAY_WRITE
    std::uint16_t write_out_buf = buffers_.allocate();
    if (write_out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit DELAY_WRITE instruction
    // DELAY_WRITE: writes input + processed*fb to buffer, outputs delayed signal
    // in0: input, in1: processed feedback, in2: feedback amount
    // in3: dry level (0xFFFF = default 0.0), in4: wet level (0xFFFF = default 1.0)
    // Note: DELAY_WRITE doesn't need rate field (time conversion done by DELAY_TAP)
    cedar::Instruction write_inst{};
    write_inst.opcode = cedar::Opcode::DELAY_WRITE;
    write_inst.out_buffer = write_out_buf;
    write_inst.inputs[0] = in_buf;
    write_inst.inputs[1] = processed_buf;
    write_inst.inputs[2] = fb_buf;
    write_inst.inputs[3] = dry_buf;
    write_inst.inputs[4] = wet_buf;
    write_inst.rate = 0;
    write_inst.state_id = delay_state_id;  // Same state_id as TAP!
    emit(write_inst);

    // Pop the outer path
    pop_path();

    node_buffers_[node] = write_out_buf;
    return write_out_buf;
}

} // namespace akkado

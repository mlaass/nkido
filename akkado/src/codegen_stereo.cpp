// Stereo signal codegen implementations
// Handles stereo(), left(), right(), pan(), width(), ms_encode(), ms_decode(), pingpong()

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include <algorithm>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::emit_push_const;
using codegen::extract_call_args;

// ============================================================================
// Stereo tracking
// ============================================================================

void CodeGenerator::register_stereo(NodeIndex node, std::uint16_t left, std::uint16_t right) {
    stereo_outputs_[node] = {left, right};
    // Also register by buffer index for variable propagation
    stereo_buffer_pairs_[left] = right;
    // Also register as multi-buffer for compatibility with existing array expansion
    multi_buffers_[node] = {left, right};
}

bool CodeGenerator::is_stereo(NodeIndex node) const {
    // First check node-based tracking
    if (stereo_outputs_.find(node) != stereo_outputs_.end()) {
        return true;
    }
    // Fallback: check if the node's buffer is a stereo left channel
    auto buf_it = node_buffers_.find(node);
    if (buf_it != node_buffers_.end()) {
        return stereo_buffer_pairs_.find(buf_it->second) != stereo_buffer_pairs_.end();
    }
    return false;
}

bool CodeGenerator::is_stereo_buffer(std::uint16_t buffer) const {
    return stereo_buffer_pairs_.find(buffer) != stereo_buffer_pairs_.end();
}

CodeGenerator::StereoBuffers CodeGenerator::get_stereo_buffers(NodeIndex node) const {
    // First try node-based tracking
    auto it = stereo_outputs_.find(node);
    if (it != stereo_outputs_.end()) {
        return it->second;
    }
    // Fallback: check buffer-based tracking
    auto buf_it = node_buffers_.find(node);
    if (buf_it != node_buffers_.end()) {
        auto pair_it = stereo_buffer_pairs_.find(buf_it->second);
        if (pair_it != stereo_buffer_pairs_.end()) {
            return {buf_it->second, pair_it->second};
        }
    }
    // Fallback: if it's a multi-buffer with exactly 2 elements, treat as stereo
    auto mb_it = multi_buffers_.find(node);
    if (mb_it != multi_buffers_.end() && mb_it->second.size() == 2) {
        return {mb_it->second[0], mb_it->second[1]};
    }
    // Not stereo - return same buffer for both (mono)
    if (buf_it != node_buffers_.end()) {
        return {buf_it->second, buf_it->second};
    }
    return {BufferAllocator::BUFFER_UNUSED, BufferAllocator::BUFFER_UNUSED};
}

CodeGenerator::StereoBuffers CodeGenerator::get_stereo_buffers_by_buffer(std::uint16_t buffer) const {
    auto pair_it = stereo_buffer_pairs_.find(buffer);
    if (pair_it != stereo_buffer_pairs_.end()) {
        return {buffer, pair_it->second};
    }
    // Not stereo - return same buffer for both (mono)
    return {buffer, buffer};
}

// ============================================================================
// Stereo handlers
// ============================================================================

// stereo(mono) -> duplicate mono to both L and R
// stereo(left, right) -> create stereo pair from two signals
std::uint16_t CodeGenerator::handle_stereo_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);

    if (!args.valid || args.nodes.empty()) {
        error("E160", "stereo() requires 1 or 2 arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t left_buf, right_buf;

    if (args.nodes.size() == 1) {
        // stereo(mono) - check if input is already stereo
        NodeIndex mono_node = args.nodes[0];
        std::uint16_t mono_buf = visit(mono_node);

        // Check stereo by both node and buffer (buffer fallback for pipe chains)
        bool input_is_stereo = is_stereo(mono_node) || is_stereo_buffer(mono_buf);

        if (input_is_stereo) {
            // Already stereo - just propagate
            StereoBuffers stereo;
            if (is_stereo(mono_node)) {
                stereo = get_stereo_buffers(mono_node);
            } else {
                stereo = get_stereo_buffers_by_buffer(mono_buf);
            }
            register_stereo(node, stereo.left, stereo.right);
            node_buffers_[node] = stereo.left;
            return stereo.left;
        }

        // Mono input - allocate two new buffers and copy
        left_buf = buffers_.allocate();
        right_buf = buffers_.allocate();

        if (left_buf == BufferAllocator::BUFFER_UNUSED ||
            right_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        // Emit COPY instructions to duplicate mono to both channels
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, left_buf, mono_buf));
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, right_buf, mono_buf));
    } else {
        // stereo(left, right) - use provided buffers directly
        left_buf = visit(args.nodes[0]);
        right_buf = visit(args.nodes[1]);
    }

    register_stereo(node, left_buf, right_buf);
    node_buffers_[node] = left_buf;
    return left_buf;
}

// left(stereo) -> extract left channel
std::uint16_t CodeGenerator::handle_left_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 1);

    if (!args.valid || args.nodes.empty()) {
        error("E161", "left() requires 1 argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex stereo_node = args.nodes[0];
    std::uint16_t buf = visit(stereo_node);

    // Check stereo by both node and buffer (buffer fallback for pipe chains)
    bool input_is_stereo = is_stereo(stereo_node) || is_stereo_buffer(buf);

    if (input_is_stereo) {
        StereoBuffers stereo;
        if (is_stereo(stereo_node)) {
            stereo = get_stereo_buffers(stereo_node);
        } else {
            stereo = get_stereo_buffers_by_buffer(buf);
        }
        node_buffers_[node] = stereo.left;
        return stereo.left;
    }

    // Not stereo - return the mono signal as-is
    node_buffers_[node] = buf;
    return buf;
}

// right(stereo) -> extract right channel
std::uint16_t CodeGenerator::handle_right_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 1);

    if (!args.valid || args.nodes.empty()) {
        error("E163", "right() requires 1 argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex stereo_node = args.nodes[0];
    std::uint16_t buf = visit(stereo_node);

    // Check stereo by both node and buffer (buffer fallback for pipe chains)
    bool input_is_stereo = is_stereo(stereo_node) || is_stereo_buffer(buf);

    if (input_is_stereo) {
        StereoBuffers stereo;
        if (is_stereo(stereo_node)) {
            stereo = get_stereo_buffers(stereo_node);
        } else {
            stereo = get_stereo_buffers_by_buffer(buf);
        }
        node_buffers_[node] = stereo.right;
        return stereo.right;
    }

    // Not stereo - return the mono signal as-is
    node_buffers_[node] = buf;
    return buf;
}

// pan(mono, position) -> create stereo from mono with panning
std::uint16_t CodeGenerator::handle_pan_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2, 2);

    if (!args.valid || args.nodes.size() < 2) {
        error("E165", "pan() requires 2 arguments: pan(mono, position)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t mono_buf = visit(args.nodes[0]);
    std::uint16_t pos_buf = visit(args.nodes[1]);

    // Allocate two adjacent output buffers for stereo
    std::uint16_t left_buf = buffers_.allocate();
    std::uint16_t right_buf = buffers_.allocate();

    if (left_buf == BufferAllocator::BUFFER_UNUSED ||
        right_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Ensure right_buf = left_buf + 1 (required by PAN opcode)
    if (right_buf != left_buf + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit PAN instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::PAN;
    inst.out_buffer = left_buf;  // L output; R is implicitly left_buf + 1
    inst.inputs[0] = mono_buf;
    inst.inputs[1] = pos_buf;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = 0;
    emit(inst);

    register_stereo(node, left_buf, right_buf);
    node_buffers_[node] = left_buf;
    return left_buf;
}

// width(stereo, amount) or width(L, R, amount) -> adjust stereo width
std::uint16_t CodeGenerator::handle_width_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2, 3);

    if (!args.valid || args.nodes.size() < 2) {
        error("E168", "width() requires 2 or 3 arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t left_buf, right_buf, width_buf;

    if (args.nodes.size() == 2) {
        // width(stereo, amount) - convenience form
        NodeIndex stereo_node = args.nodes[0];
        visit(stereo_node);
        width_buf = visit(args.nodes[1]);

        if (is_stereo(stereo_node)) {
            auto stereo = get_stereo_buffers(stereo_node);
            left_buf = stereo.left;
            right_buf = stereo.right;
        } else {
            // Mono input - treat as stereo with same signal on both channels
            auto it = node_buffers_.find(stereo_node);
            if (it == node_buffers_.end()) {
                error("E167", "width() first argument is not a signal", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }
            left_buf = it->second;
            right_buf = it->second;
        }
    } else {
        // width(L, R, amount) - explicit form
        left_buf = visit(args.nodes[0]);
        right_buf = visit(args.nodes[1]);
        width_buf = visit(args.nodes[2]);
    }

    // Allocate output buffers
    std::uint16_t out_left = buffers_.allocate();
    std::uint16_t out_right = buffers_.allocate();

    if (out_left == BufferAllocator::BUFFER_UNUSED ||
        out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit WIDTH instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::WIDTH;
    inst.out_buffer = out_left;
    inst.inputs[0] = left_buf;
    inst.inputs[1] = right_buf;
    inst.inputs[2] = width_buf;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = 0;
    emit(inst);

    register_stereo(node, out_left, out_right);
    node_buffers_[node] = out_left;
    return out_left;
}

// ms_encode(stereo) or ms_encode(L, R) -> convert to mid/side
std::uint16_t CodeGenerator::handle_ms_encode_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);

    if (!args.valid || args.nodes.empty()) {
        error("E170", "ms_encode() requires 1 or 2 arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t left_buf, right_buf;

    if (args.nodes.size() == 1) {
        // ms_encode(stereo) - convenience form
        NodeIndex stereo_node = args.nodes[0];
        visit(stereo_node);

        if (is_stereo(stereo_node)) {
            auto stereo = get_stereo_buffers(stereo_node);
            left_buf = stereo.left;
            right_buf = stereo.right;
        } else {
            // Mono - mid = signal, side = 0
            auto it = node_buffers_.find(stereo_node);
            if (it == node_buffers_.end()) {
                error("E169", "ms_encode() argument is not a signal", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }
            left_buf = it->second;
            right_buf = it->second;
        }
    } else {
        // ms_encode(L, R) - explicit form
        left_buf = visit(args.nodes[0]);
        right_buf = visit(args.nodes[1]);
    }

    // Allocate output buffers (mid, side)
    std::uint16_t out_mid = buffers_.allocate();
    std::uint16_t out_side = buffers_.allocate();

    if (out_mid == BufferAllocator::BUFFER_UNUSED ||
        out_side == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (out_side != out_mid + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit MS_ENCODE instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::MS_ENCODE;
    inst.out_buffer = out_mid;
    inst.inputs[0] = left_buf;
    inst.inputs[1] = right_buf;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = 0;
    emit(inst);

    // Register as stereo-like (mid/side pair)
    register_stereo(node, out_mid, out_side);
    node_buffers_[node] = out_mid;
    return out_mid;
}

// ms_decode(ms) or ms_decode(M, S) -> convert mid/side to stereo
std::uint16_t CodeGenerator::handle_ms_decode_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);

    if (!args.valid || args.nodes.empty()) {
        error("E172", "ms_decode() requires 1 or 2 arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t mid_buf, side_buf;

    if (args.nodes.size() == 1) {
        // ms_decode(ms) - convenience form (input is stereo-like mid/side pair)
        NodeIndex ms_node = args.nodes[0];
        visit(ms_node);

        if (is_stereo(ms_node)) {
            auto stereo = get_stereo_buffers(ms_node);
            mid_buf = stereo.left;   // Treat as M
            side_buf = stereo.right;  // Treat as S
        } else {
            // Mono - just return as-is (mid only, no side info)
            auto it = node_buffers_.find(ms_node);
            if (it == node_buffers_.end()) {
                error("E171", "ms_decode() argument is not a signal", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }
            mid_buf = it->second;
            // Allocate a zero buffer for side
            side_buf = buffers_.allocate();
            if (side_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return BufferAllocator::BUFFER_UNUSED;
            }
            cedar::Instruction zero_inst{};
            zero_inst.opcode = cedar::Opcode::PUSH_CONST;
            zero_inst.out_buffer = side_buf;
            zero_inst.inputs[0] = 0xFFFF;
            zero_inst.inputs[1] = 0xFFFF;
            zero_inst.inputs[2] = 0xFFFF;
            zero_inst.inputs[3] = 0xFFFF;
            encode_const_value(zero_inst, 0.0f);
            emit(zero_inst);
        }
    } else {
        // ms_decode(M, S) - explicit form
        mid_buf = visit(args.nodes[0]);
        side_buf = visit(args.nodes[1]);
    }

    // Allocate output buffers (left, right)
    std::uint16_t out_left = buffers_.allocate();
    std::uint16_t out_right = buffers_.allocate();

    if (out_left == BufferAllocator::BUFFER_UNUSED ||
        out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit MS_DECODE instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::MS_DECODE;
    inst.out_buffer = out_left;
    inst.inputs[0] = mid_buf;
    inst.inputs[1] = side_buf;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = 0;
    emit(inst);

    register_stereo(node, out_left, out_right);
    node_buffers_[node] = out_left;
    return out_left;
}

// pingpong(stereo, time, fb) or pingpong(L, R, time, fb, width?) -> true stereo ping-pong delay
std::uint16_t CodeGenerator::handle_pingpong_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3, 5);

    if (!args.valid || args.nodes.size() < 3) {
        error("E175", "pingpong() requires at least 3 arguments", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t left_buf, right_buf, time_buf, fb_buf, width_buf = BufferAllocator::BUFFER_UNUSED;

    // Check first argument to determine form
    NodeIndex first_node = args.nodes[0];
    std::uint16_t first_buf = visit(first_node);

    // Check stereo by both node and buffer
    bool first_is_stereo = is_stereo(first_node) || is_stereo_buffer(first_buf);

    if (first_is_stereo) {
        // pingpong(stereo, time, fb, width?)
        StereoBuffers stereo;
        if (is_stereo(first_node)) {
            stereo = get_stereo_buffers(first_node);
        } else {
            // Fall back to buffer-based lookup
            stereo = get_stereo_buffers_by_buffer(first_buf);
        }
        left_buf = stereo.left;
        right_buf = stereo.right;

        time_buf = visit(args.nodes[1]);
        fb_buf = visit(args.nodes[2]);

        if (args.nodes.size() >= 4) {
            width_buf = visit(args.nodes[3]);
        }
    } else {
        // pingpong(L, R, time, fb, width?)
        if (args.nodes.size() < 4) {
            error("E173", "pingpong() requires at least 4 arguments: pingpong(L, R, time, fb)", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        auto it = node_buffers_.find(first_node);
        if (it == node_buffers_.end()) {
            error("E174", "pingpong() first argument is not a signal", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
        left_buf = it->second;
        right_buf = visit(args.nodes[1]);
        time_buf = visit(args.nodes[2]);
        fb_buf = visit(args.nodes[3]);

        if (args.nodes.size() >= 5) {
            width_buf = visit(args.nodes[4]);
        }
    }

    // Allocate output buffers
    std::uint16_t out_left = buffers_.allocate();
    std::uint16_t out_right = buffers_.allocate();

    if (out_left == BufferAllocator::BUFFER_UNUSED ||
        out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Generate state_id for the stateful delay
    std::uint32_t count = call_counters_["pingpong"]++;
    push_path("pingpong#" + std::to_string(count));
    std::uint32_t state_id = compute_state_id();
    pop_path();

    // Emit DELAY_PINGPONG instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::DELAY_PINGPONG;
    inst.out_buffer = out_left;
    inst.inputs[0] = left_buf;
    inst.inputs[1] = right_buf;
    inst.inputs[2] = time_buf;
    inst.inputs[3] = fb_buf;
    inst.inputs[4] = width_buf;  // BUFFER_UNUSED if not provided
    inst.rate = 0;
    inst.state_id = state_id;
    emit(inst);

    register_stereo(node, out_left, out_right);
    node_buffers_[node] = out_left;
    return out_left;
}

}  // namespace akkado

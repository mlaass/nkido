// Visualization exposure codegen implementations
// Handles pianoroll(), oscilloscope(), waveform(), spectrum() for UI auto-generation
// These functions pass signal through while creating visualization metadata

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <algorithm>

namespace akkado {

using codegen::unwrap_argument;

// Helper: Extract optional string name argument
static std::string extract_name_arg(const AstArena& arena, NodeIndex arg_node,
                                     const std::string& default_name) {
    if (arg_node == NULL_NODE) {
        return default_name;
    }

    NodeIndex value_node = unwrap_argument(arena, arg_node);
    const Node& n = arena[value_node];

    if (n.type == NodeType::StringLit) {
        return std::string(n.as_string());
    }

    return default_name;
}

// Helper: Get next sibling argument
static NodeIndex next_arg(const AstArena& arena, NodeIndex arg_node) {
    if (arg_node == NULL_NODE) return NULL_NODE;
    return arena[arg_node].next_sibling;
}

// ============================================================================
// pianoroll(signal, name?) - Piano roll pattern visualization
// ============================================================================

std::uint16_t CodeGenerator::handle_pianoroll_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E170", "pianoroll() requires a signal argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer - pass through
    std::uint16_t signal_buf = visit(signal_node);
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Piano Roll");

    // 4. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::PianoRoll;
    decl.state_id = 0;  // Not used for piano roll
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;

    // 5. Try to find corresponding pattern state_init for linking
    // The signal may come from a pattern - search state_inits for matching source location
    // This enables the piano roll to access pattern events without duplication
    decl.pattern_state_init_index = -1;
    for (std::size_t i = 0; i < state_inits_.size(); ++i) {
        const auto& init = state_inits_[i];
        if (init.type == StateInitData::Type::SequenceProgram) {
            // Check if pattern location is within or near this call
            // For now, use most recent pattern if signal comes from a pattern node
            decl.pattern_state_init_index = static_cast<std::int32_t>(i);
            // Store the pattern's state_id for direct lookup on the JS side
            decl.state_id = init.state_id;
        }
    }

    // 6. Add to declarations (no deduplication - multiple piano rolls allowed)
    viz_decls_.push_back(std::move(decl));

    // 7. Signal passes through unchanged
    node_buffers_[node] = signal_buf;
    return signal_buf;
}

// ============================================================================
// oscilloscope(signal, name?) - Time-domain oscilloscope visualization
// ============================================================================

std::uint16_t CodeGenerator::handle_oscilloscope_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E171", "oscilloscope() requires a signal argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node);
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Oscilloscope");

    // 4. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("oscilloscope");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 5. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Oscilloscope;
    decl.state_id = state_id;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 6. Emit PROBE opcode to capture signal data
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction probe_inst{};
    probe_inst.opcode = cedar::Opcode::PROBE;
    probe_inst.out_buffer = out_buf;
    probe_inst.inputs[0] = signal_buf;
    probe_inst.inputs[1] = 0xFFFF;
    probe_inst.inputs[2] = 0xFFFF;
    probe_inst.inputs[3] = 0xFFFF;
    probe_inst.inputs[4] = 0xFFFF;
    probe_inst.state_id = state_id;
    emit(probe_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}

// ============================================================================
// waveform(signal, name?) - Time-domain waveform visualization (longer window)
// ============================================================================

std::uint16_t CodeGenerator::handle_waveform_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E172", "waveform() requires a signal argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node);
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Waveform");

    // 4. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("waveform");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 5. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Waveform;
    decl.state_id = state_id;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 6. Emit PROBE opcode to capture signal data
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction probe_inst{};
    probe_inst.opcode = cedar::Opcode::PROBE;
    probe_inst.out_buffer = out_buf;
    probe_inst.inputs[0] = signal_buf;
    probe_inst.inputs[1] = 0xFFFF;
    probe_inst.inputs[2] = 0xFFFF;
    probe_inst.inputs[3] = 0xFFFF;
    probe_inst.inputs[4] = 0xFFFF;
    probe_inst.state_id = state_id;
    emit(probe_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}

// ============================================================================
// spectrum(signal, name?) - Frequency-domain FFT visualization
// ============================================================================

std::uint16_t CodeGenerator::handle_spectrum_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E173", "spectrum() requires a signal argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node);
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Spectrum");

    // 4. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("spectrum");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 5. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Spectrum;
    decl.state_id = state_id;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 6. Emit PROBE opcode to capture signal data
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction probe_inst{};
    probe_inst.opcode = cedar::Opcode::PROBE;
    probe_inst.out_buffer = out_buf;
    probe_inst.inputs[0] = signal_buf;
    probe_inst.inputs[1] = 0xFFFF;
    probe_inst.inputs[2] = 0xFFFF;
    probe_inst.inputs[3] = 0xFFFF;
    probe_inst.inputs[4] = 0xFFFF;
    probe_inst.state_id = state_id;
    emit(probe_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}

}  // namespace akkado

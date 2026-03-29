// Visualization exposure codegen implementations
// Handles pianoroll(), oscilloscope(), waveform(), spectrum() for UI auto-generation
// These functions pass signal through while creating visualization metadata

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <algorithm>
#include <cstdio>  // For snprintf - locale-free alternative to ostringstream

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

// Helper: Extract options record and serialize to JSON
// Supports: {width: 300, height: 80} - numeric fields only for now
// Note: Uses manual string building instead of ostringstream to avoid
// std::locale which requires pthreads (not available in AudioWorklet)
static std::string extract_options_json(const AstArena& arena, NodeIndex arg_node) {
    if (arg_node == NULL_NODE) {
        return "";
    }

    NodeIndex value_node = unwrap_argument(arena, arg_node);
    const Node& n = arena[value_node];

    if (n.type != NodeType::RecordLit) {
        return "";
    }

    // Build JSON from record fields using manual string concatenation
    std::string json = "{";

    bool first = true;
    NodeIndex field_node = n.first_child;
    while (field_node != NULL_NODE) {
        const Node& field = arena[field_node];

        // Field nodes are Argument type with RecordFieldData
        if (field.type == NodeType::Argument &&
            std::holds_alternative<Node::RecordFieldData>(field.data)) {

            const auto& field_data = field.as_record_field();
            NodeIndex value_node_child = field.first_child;

            if (value_node_child != NULL_NODE) {
                const Node& val = arena[value_node_child];

                // Extract numeric value
                if (val.type == NodeType::NumberLit) {
                    if (!first) json += ",";
                    first = false;

                    // Format: "fieldname":number
                    // Use snprintf for locale-independent float formatting
                    char num_buf[32];
                    std::snprintf(num_buf, sizeof(num_buf), "%g", val.as_number());
                    json += "\"";
                    json += field_data.name;
                    json += "\":";
                    json += num_buf;
                }
                // Extract string value (e.g., {gradient: "magma"})
                else if (val.type == NodeType::StringLit) {
                    if (!first) json += ",";
                    first = false;

                    json += "\"";
                    json += field_data.name;
                    json += "\":\"";
                    json += val.as_string();
                    json += "\"";
                }
                // Extract boolean value (e.g., {logScale: true})
                else if (val.type == NodeType::BoolLit) {
                    if (!first) json += ",";
                    first = false;

                    json += "\"";
                    json += field_data.name;
                    json += "\":";
                    json += val.as_bool() ? "true" : "false";
                }
            }
        }

        field_node = arena[field_node].next_sibling;
    }

    json += "}";

    // Return empty string if no fields were extracted
    if (first) {
        return "";
    }

    return json;
}

// Helper: Extract FFT size from options JSON and return log2 value
// Returns default_log2 if "fft" key is not found
static std::uint8_t extract_fft_log2(const std::string& options_json, std::uint8_t default_log2 = 10) {
    if (options_json.empty()) return default_log2;

    auto pos = options_json.find("\"fft\":");
    if (pos == std::string::npos) return default_log2;

    int fft_val = std::atoi(options_json.c_str() + pos + 6);
    if (fft_val == 256)  return 8;
    if (fft_val == 512)  return 9;
    if (fft_val == 1024) return 10;
    if (fft_val == 2048) return 11;
    return default_log2;
}

// ============================================================================
// pianoroll(signal, name?, options?) - Piano roll pattern visualization
// ============================================================================

TypedValue CodeGenerator::handle_pianoroll_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E170", "pianoroll() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer - pass through
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Piano Roll");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    std::string options_json = extract_options_json(ast_->arena, options_arg);

    // 5. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::PianoRoll;
    decl.state_id = 0;  // Not used for piano roll
    decl.options_json = options_json;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;

    // 6. Try to find corresponding pattern state_init for linking
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

    // 7. Add to declarations (no deduplication - multiple piano rolls allowed)
    viz_decls_.push_back(std::move(decl));

    // 8. Signal passes through unchanged
    return cache_and_return(node, TypedValue::signal(signal_buf));
}

// ============================================================================
// oscilloscope(signal, name?, options?) - Time-domain oscilloscope visualization
// ============================================================================

TypedValue CodeGenerator::handle_oscilloscope_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E171", "oscilloscope() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Oscilloscope");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    std::string options_json = extract_options_json(ast_->arena, options_arg);

    // 5. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("oscilloscope");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Oscilloscope;
    decl.state_id = state_id;
    decl.options_json = options_json;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit PROBE opcode to capture signal data
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
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

    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// waveform(signal, name?, options?) - Time-domain waveform visualization (longer window)
// ============================================================================

TypedValue CodeGenerator::handle_waveform_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E172", "waveform() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Waveform");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    std::string options_json = extract_options_json(ast_->arena, options_arg);

    // 5. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("waveform");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Waveform;
    decl.state_id = state_id;
    decl.options_json = options_json;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit PROBE opcode to capture signal data
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
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

    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// spectrum(signal, name?, options?) - Frequency-domain FFT visualization
// ============================================================================

TypedValue CodeGenerator::handle_spectrum_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E173", "spectrum() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Spectrum");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    std::string options_json = extract_options_json(ast_->arena, options_arg);

    // 5. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("spectrum");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Spectrum;
    decl.state_id = state_id;
    decl.options_json = options_json;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit FFT_PROBE opcode (migrated from PROBE for WASM FFT)
    std::uint8_t fft_log2 = extract_fft_log2(options_json);

    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    cedar::Instruction probe_inst{};
    probe_inst.opcode = cedar::Opcode::FFT_PROBE;
    probe_inst.rate = fft_log2;
    probe_inst.out_buffer = out_buf;
    probe_inst.inputs[0] = signal_buf;
    probe_inst.inputs[1] = 0xFFFF;
    probe_inst.inputs[2] = 0xFFFF;
    probe_inst.inputs[3] = 0xFFFF;
    probe_inst.inputs[4] = 0xFFFF;
    probe_inst.state_id = state_id;
    emit(probe_inst);

    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// waterfall(signal, name?, options?) - Spectral waterfall visualization
// ============================================================================

TypedValue CodeGenerator::handle_waterfall_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E174", "waterfall() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Waterfall");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    std::string options_json = extract_options_json(ast_->arena, options_arg);

    // 5. Generate state_id for FFT probe
    push_path("waterfall");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Waterfall;
    decl.state_id = state_id;
    decl.options_json = options_json;
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit FFT_PROBE opcode
    std::uint8_t fft_log2 = extract_fft_log2(options_json);

    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    cedar::Instruction probe_inst{};
    probe_inst.opcode = cedar::Opcode::FFT_PROBE;
    probe_inst.rate = fft_log2;
    probe_inst.out_buffer = out_buf;
    probe_inst.inputs[0] = signal_buf;
    probe_inst.inputs[1] = 0xFFFF;
    probe_inst.inputs[2] = 0xFFFF;
    probe_inst.inputs[3] = 0xFFFF;
    probe_inst.inputs[4] = 0xFFFF;
    probe_inst.state_id = state_id;
    emit(probe_inst);

    return cache_and_return(node, TypedValue::signal(out_buf));
}

}  // namespace akkado

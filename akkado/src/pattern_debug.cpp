#include <akkado/pattern_debug.hpp>
#include <cstdio>  // snprintf - locale-free alternative to ostringstream

namespace akkado {

namespace {

/// Escape a string for JSON
std::string escape_json(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (char c : sv) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

/// Get modifier type as string
const char* modifier_type_str(Node::MiniModifierType type) {
    switch (type) {
        case Node::MiniModifierType::Speed: return "Speed";
        case Node::MiniModifierType::Slow: return "Slow";
        case Node::MiniModifierType::Weight: return "Weight";
        case Node::MiniModifierType::Repeat: return "Repeat";
        case Node::MiniModifierType::Chance: return "Chance";
    }
    return "Unknown";
}

/// Get atom kind as string
const char* atom_kind_str(Node::MiniAtomKind kind) {
    switch (kind) {
        case Node::MiniAtomKind::Pitch: return "Pitch";
        case Node::MiniAtomKind::Sample: return "Sample";
        case Node::MiniAtomKind::Rest: return "Rest";
        case Node::MiniAtomKind::Elongate: return "Elongate";
        case Node::MiniAtomKind::Chord: return "Chord";
        case Node::MiniAtomKind::CurveLevel: return "CurveLevel";
        case Node::MiniAtomKind::CurveRamp: return "CurveRamp";
    }
    return "Unknown";
}

/// Get sequence mode as string
const char* sequence_mode_str(cedar::SequenceMode mode) {
    switch (mode) {
        case cedar::SequenceMode::NORMAL: return "NORMAL";
        case cedar::SequenceMode::ALTERNATE: return "ALTERNATE";
        case cedar::SequenceMode::RANDOM: return "RANDOM";
    }
    return "UNKNOWN";
}

/// Serialize a single AST node to JSON (recursive)
void serialize_node(std::string& json, NodeIndex idx, const AstArena& arena) {
    if (idx == NULL_NODE || !arena.valid(idx)) {
        json += "null";
        return;
    }

    char num_buf[32];
    const Node& node = arena[idx];
    json += "{";
    json += "\"type\":\"";
    json += node_type_name(node.type);
    json += "\"";

    // Add source location
    json += ",\"location\":{\"offset\":";
    std::snprintf(num_buf, sizeof(num_buf), "%u", node.location.offset);
    json += num_buf;
    json += ",\"length\":";
    std::snprintf(num_buf, sizeof(num_buf), "%u", node.location.length);
    json += num_buf;
    json += "}";

    // Add node-specific data
    switch (node.type) {
        case NodeType::MiniAtom: {
            const auto& data = node.as_mini_atom();
            json += ",\"kind\":\"";
            json += atom_kind_str(data.kind);
            json += "\"";
            if (data.kind == Node::MiniAtomKind::Pitch) {
                json += ",\"midi\":";
                std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(data.midi_note));
                json += num_buf;
                if (data.micro_offset != 0) {
                    json += ",\"microOffset\":";
                    std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(data.micro_offset));
                    json += num_buf;
                }
            } else if (data.kind == Node::MiniAtomKind::Sample) {
                json += ",\"sampleName\":\"";
                json += escape_json(data.sample_name);
                json += "\"";
                if (data.sample_variant > 0) {
                    json += ",\"variant\":";
                    std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(data.sample_variant));
                    json += num_buf;
                }
            } else if (data.kind == Node::MiniAtomKind::CurveLevel) {
                json += ",\"value\":";
                std::snprintf(num_buf, sizeof(num_buf), "%g", data.curve_value);
                json += num_buf;
                if (data.curve_smooth) {
                    json += ",\"smooth\":true";
                }
            }
            // CurveRamp has no additional data fields
            break;
        }

        case NodeType::MiniEuclidean: {
            const auto& data = node.as_mini_euclidean();
            json += ",\"hits\":";
            std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(data.hits));
            json += num_buf;
            json += ",\"steps\":";
            std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(data.steps));
            json += num_buf;
            json += ",\"rotation\":";
            std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(data.rotation));
            json += num_buf;
            break;
        }

        case NodeType::MiniModified: {
            const auto& data = node.as_mini_modifier();
            json += ",\"modifier\":\"";
            json += modifier_type_str(data.modifier_type);
            json += "\"";
            json += ",\"value\":";
            std::snprintf(num_buf, sizeof(num_buf), "%g", data.value);
            json += num_buf;
            break;
        }

        case NodeType::MiniPolymeter: {
            const auto& data = node.as_mini_polymeter();
            json += ",\"stepCount\":";
            std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(data.step_count));
            json += num_buf;
            break;
        }

        default:
            // Other node types don't have extra data in mini-notation context
            break;
    }

    // Add children
    if (node.first_child != NULL_NODE) {
        json += ",\"children\":[";
        bool first = true;
        NodeIndex child = node.first_child;
        while (child != NULL_NODE && arena.valid(child)) {
            if (!first) json += ",";
            first = false;
            serialize_node(json, child, arena);
            child = arena[child].next_sibling;
        }
        json += "]";
    }

    json += "}";
}

} // anonymous namespace

std::string serialize_mini_ast_json(NodeIndex root, const AstArena& arena) {
    std::string json;
    json.reserve(1024);  // Reasonable initial capacity for AST
    serialize_node(json, root, arena);
    return json;
}

std::string serialize_sequences_json(
    const std::vector<cedar::Sequence>& sequences,
    const std::vector<std::vector<cedar::Event>>& sequence_events) {

    std::string json;
    json.reserve(4096);  // Reasonable initial capacity for sequences
    char num_buf[32];

    json += "{\"sequences\":[";

    for (std::size_t i = 0; i < sequences.size(); ++i) {
        if (i > 0) json += ",";

        const auto& seq = sequences[i];
        json += "{";
        json += "\"id\":";
        std::snprintf(num_buf, sizeof(num_buf), "%zu", i);
        json += num_buf;
        json += ",\"mode\":\"";
        json += sequence_mode_str(seq.mode);
        json += "\"";
        json += ",\"duration\":";
        std::snprintf(num_buf, sizeof(num_buf), "%g", seq.duration);
        json += num_buf;
        json += ",\"step\":";
        std::snprintf(num_buf, sizeof(num_buf), "%u", seq.step);
        json += num_buf;
        json += ",\"events\":[";

        // Use sequence_events if available, otherwise fall back to seq.events pointer
        const cedar::Event* events = nullptr;
        std::uint32_t num_events = 0;

        if (i < sequence_events.size() && !sequence_events[i].empty()) {
            events = sequence_events[i].data();
            num_events = static_cast<std::uint32_t>(sequence_events[i].size());
        } else if (seq.events != nullptr) {
            events = seq.events;
            num_events = seq.num_events;
        }

        for (std::uint32_t j = 0; j < num_events; ++j) {
            if (j > 0) json += ",";
            const auto& e = events[j];
            json += "{";
            const char* type_str = (e.type == cedar::EventType::DATA)
                ? (e.num_values == 0 ? "REST" : "DATA")
                : "SUB_SEQ";
            json += "\"type\":\"";
            json += type_str;
            json += "\"";
            json += ",\"time\":";
            std::snprintf(num_buf, sizeof(num_buf), "%g", e.time);
            json += num_buf;
            json += ",\"duration\":";
            std::snprintf(num_buf, sizeof(num_buf), "%g", e.duration);
            json += num_buf;
            json += ",\"chance\":";
            std::snprintf(num_buf, sizeof(num_buf), "%g", e.chance);
            json += num_buf;
            json += ",\"sourceOffset\":";
            std::snprintf(num_buf, sizeof(num_buf), "%u", e.source_offset);
            json += num_buf;
            json += ",\"sourceLength\":";
            std::snprintf(num_buf, sizeof(num_buf), "%u", e.source_length);
            json += num_buf;

            if (e.type == cedar::EventType::DATA) {
                json += ",\"numValues\":";
                std::snprintf(num_buf, sizeof(num_buf), "%d", static_cast<int>(e.num_values));
                json += num_buf;
                json += ",\"values\":[";
                for (std::uint8_t k = 0; k < e.num_values; ++k) {
                    if (k > 0) json += ",";
                    std::snprintf(num_buf, sizeof(num_buf), "%g", e.values[k]);
                    json += num_buf;
                }
                json += "]";
            } else {
                json += ",\"seqId\":";
                std::snprintf(num_buf, sizeof(num_buf), "%u", e.seq_id);
                json += num_buf;
            }
            json += "}";
        }

        json += "]}";
    }

    json += "]}";
    return json;
}

} // namespace akkado

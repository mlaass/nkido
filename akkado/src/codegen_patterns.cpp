// Pattern and chord codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/pattern_debug.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <bit>
#include <cmath>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::emit_zero;

// ============================================================================
// SequenceCompiler - Converts mini-notation AST to Sequence/Event format
// ============================================================================
// This compiles the AST into sequences that can be evaluated at runtime
// using the new simplified query_sequence() function.
//
// Key mappings:
//   [a b c]    -> NORMAL sequence (events at subdivided times)
//   <a b c>    -> ALTERNATE sequence (one event per query, advances step)
//   a | b | c  -> RANDOM sequence (pick one randomly)
//   *N         -> Speed modifier (creates N SUB_SEQ events for alternates)
//   !N         -> Repeat modifier (duplicates events)
//   ?N         -> Chance modifier (sets event.chance)

class SequenceCompiler {
public:
    explicit SequenceCompiler(const AstArena& arena, SampleRegistry* sample_registry = nullptr)
        : arena_(arena), sample_registry_(sample_registry) {}

    // Set base offset for computing pattern-relative source offsets
    void set_pattern_base_offset(std::uint32_t offset) {
        pattern_base_offset_ = offset;
    }

    // Compile a pattern AST into Sequence format
    // Returns true on success, false if compilation fails
    bool compile(NodeIndex root) {
        sequences_.clear();
        sequence_events_.clear();
        sample_mappings_.clear();
        current_seq_idx_ = 0;
        total_events_ = 0;
        if (root == NULL_NODE) return false;

        // Create root sequence at index 0 (query_pattern always starts from sequence 0)
        cedar::Sequence root_seq;
        root_seq.mode = cedar::SequenceMode::NORMAL;
        root_seq.duration = 1.0f;  // Normalized to 1.0, scaled by cycle_length later

        // Reserve slot 0 for root - sub-sequences will be added at indices 1+
        sequences_.push_back(root_seq);
        sequence_events_.push_back({});  // Empty event vector for root

        compile_into_sequence(root, 0, 0.0f, 1.0f);

        if (sequence_events_[0].empty()) return false;

        // Update sequences with pointers to their event vectors and counts
        finalize_sequences();
        return true;
    }

    // Get the compiled sequences (with pointers set up)
    const std::vector<cedar::Sequence>& sequences() const { return sequences_; }

    // Get the event vectors (for storage in StateInitData)
    const std::vector<std::vector<cedar::Event>>& sequence_events() const { return sequence_events_; }

    // Get total event count
    std::uint32_t total_events() const { return total_events_; }

    // Check if pattern contains samples (vs pitch)
    bool is_sample_pattern() const { return is_sample_pattern_; }

    // Register required samples
    void collect_samples(std::set<std::string>& required) const {
        for (const auto& name : sample_names_) {
            required.insert(name);
        }
    }

    // Get sample mappings for deferred resolution
    const std::vector<SequenceSampleMapping>& sample_mappings() const {
        return sample_mappings_;
    }

    // Get maximum number of values per event (for polyphonic chord support)
    // Returns 1 for monophonic patterns, >1 for patterns with chords
    std::uint8_t max_voices() const {
        std::uint8_t max = 1;
        for (const auto& seq : sequence_events_) {
            for (const auto& e : seq) {
                if (e.num_values > max) max = e.num_values;
            }
        }
        return max;
    }

    // Count top-level elements in a pattern (each element = 1 beat)
    // This determines cycle_length: pattern "a <b c> d" has 3 top-level elements
    std::uint32_t count_top_level_elements(NodeIndex node) {
        if (node == NULL_NODE) return 1;
        const Node& n = arena_[node];

        // For MiniPattern, count children (with repeat expansion)
        if (n.type == NodeType::MiniPattern) {
            std::uint32_t count = 0;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                count += static_cast<std::uint32_t>(get_node_repeat(child));
                child = arena_[child].next_sibling;
            }
            return count > 0 ? count : 1;
        }

        // Single element
        return 1;
    }

private:
    // Finalize sequences after compilation
    // Sets up the Sequence structs to point to their event vectors
    void finalize_sequences() {
        for (std::size_t i = 0; i < sequences_.size(); ++i) {
            auto& seq = sequences_[i];
            auto& events = sequence_events_[i];
            if (!events.empty()) {
                seq.events = events.data();
                seq.num_events = static_cast<std::uint32_t>(events.size());
                seq.capacity = static_cast<std::uint32_t>(events.size());
                total_events_ += seq.num_events;
            } else {
                seq.events = nullptr;
                seq.num_events = 0;
                seq.capacity = 0;
            }
        }
    }

    // Add event to a sequence by index
    void add_event_to_sequence(std::uint16_t seq_idx, const cedar::Event& e) {
        if (seq_idx < sequence_events_.size()) {
            sequence_events_[seq_idx].push_back(e);
        }
    }

    // Check if a node is "compound" (produces multiple events when compiled)
    // Such nodes need to be wrapped in a sub-sequence when added to ALTERNATE/RANDOM
    bool is_compound_node(NodeIndex idx) const {
        if (idx == NULL_NODE) return false;
        const Node& n = arena_[idx];
        // Unwrap modifiers to check the underlying node
        if (n.type == NodeType::MiniModified) {
            return is_compound_node(n.first_child);
        }
        switch (n.type) {
            case NodeType::MiniGroup:
            case NodeType::MiniPattern:
            case NodeType::MiniPolyrhythm:
            case NodeType::MiniPolymeter:
            case NodeType::MiniEuclidean:
                return true;
            default:
                return false;
        }
    }

    // Compile a child into an ALTERNATE or RANDOM sequence
    // If the child is compound, wrap it in a NORMAL sub-sequence first
    void compile_alternate_child(NodeIndex child, std::uint16_t parent_seq_idx) {
        if (is_compound_node(child)) {
            // Create a NORMAL sub-sequence to hold the compound child
            std::uint16_t sub_seq_idx = create_sub_sequence(cedar::SequenceMode::NORMAL);

            std::uint16_t saved_seq_idx = current_seq_idx_;
            current_seq_idx_ = sub_seq_idx;
            compile_into_sequence(child, sub_seq_idx, 0.0f, 1.0f);
            current_seq_idx_ = saved_seq_idx;

            if (sequence_events_[sub_seq_idx].empty()) return;

            // Add SUB_SEQ event pointing to the wrapped sequence
            cedar::Event e;
            e.type = cedar::EventType::SUB_SEQ;
            e.time = 0.0f;
            e.duration = 1.0f;
            e.chance = 1.0f;
            e.seq_id = sub_seq_idx;
            add_event_to_sequence(parent_seq_idx, e);
        } else {
            // Simple atom - compile directly
            compile_into_sequence(child, parent_seq_idx, 0.0f, 1.0f);
        }
    }

    // Compile a node into events within an existing sequence
    // seq_idx: index of the target sequence
    // time_offset: where in the parent's time span this starts (0.0-1.0)
    // time_span: how much of the parent's time span this uses (0.0-1.0)
    void compile_into_sequence(NodeIndex ast_idx, std::uint16_t seq_idx,
                                float time_offset, float time_span) {
        if (ast_idx == NULL_NODE) return;

        const Node& n = arena_[ast_idx];

        switch (n.type) {
            case NodeType::MiniPattern:
                compile_pattern_node(ast_idx, n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniAtom:
                compile_atom_event(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniGroup:
                compile_group_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniSequence:
                compile_alternate_sequence(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniPolyrhythm:
                compile_polyrhythm_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniPolymeter:
                // Treat polymeter as group for now
                compile_group_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniChoice:
                compile_choice_sequence(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniEuclidean:
                compile_euclidean_events(n, seq_idx, time_offset, time_span);
                break;
            case NodeType::MiniModified:
                compile_modified_node(n, seq_idx, time_offset, time_span);
                break;
            default:
                // Unknown node type - skip
                break;
        }
    }

    // MiniPattern: root containing children (sequential concatenation)
    void compile_pattern_node(NodeIndex /*ast_idx*/, const Node& n, std::uint16_t seq_idx,
                               float time_offset, float time_span) {
        // Count children and their weights
        std::vector<NodeIndex> children;
        std::vector<float> weights;
        float total_weight = 0.0f;

        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            float weight = get_node_weight(child);
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                children.push_back(child);
                weights.push_back(weight);
                total_weight += weight;
            }
            child = arena_[child].next_sibling;
        }

        if (children.empty()) return;
        if (total_weight <= 0.0f) total_weight = static_cast<float>(children.size());

        // Subdivide time among children
        float accumulated_time = 0.0f;
        for (std::size_t i = 0; i < children.size(); ++i) {
            float child_span = (weights[i] / total_weight) * time_span;
            float child_offset = time_offset + accumulated_time;
            compile_into_sequence(children[i], seq_idx, child_offset, child_span);
            accumulated_time += child_span;
        }
    }

    // MiniAtom: single note, sample, chord, or rest -> DATA event
    void compile_atom_event(const Node& n, std::uint16_t seq_idx,
                            float time_offset, float time_span) {
        const auto& atom_data = n.as_mini_atom();

        cedar::Event e;
        e.type = cedar::EventType::DATA;
        e.time = time_offset;
        e.duration = time_span;
        e.chance = 1.0f;
        e.num_values = 1;
        // Use pattern-relative offset for UI highlighting
        e.source_offset = static_cast<std::uint16_t>(n.location.offset - pattern_base_offset_);
        e.source_length = static_cast<std::uint16_t>(n.location.length);

        if (atom_data.kind == Node::MiniAtomKind::Rest) {
            // Rest: emit event with num_values=0 for UI highlighting (no trigger/sound)
            e.num_values = 0;
            add_event_to_sequence(seq_idx, e);
            return;
        }

        if (atom_data.kind == Node::MiniAtomKind::Pitch) {
            // Convert MIDI note to frequency
            float freq = 440.0f * std::pow(2.0f,
                (static_cast<float>(atom_data.midi_note) - 69.0f) / 12.0f);
            e.values[0] = freq;
        } else if (atom_data.kind == Node::MiniAtomKind::Chord) {
            // Chord symbol: expand intervals to frequencies
            // Root MIDI is at octave 4 by default
            int root_midi = static_cast<int>(atom_data.chord_root_midi);
            std::size_t num_notes = std::min(atom_data.chord_intervals.size(),
                                              static_cast<std::size_t>(8));  // Max 8 values
            e.num_values = static_cast<std::uint8_t>(num_notes);

            for (std::size_t i = 0; i < num_notes; ++i) {
                int midi = root_midi + static_cast<int>(atom_data.chord_intervals[i]);
                float freq = 440.0f * std::pow(2.0f,
                    (static_cast<float>(midi) - 69.0f) / 12.0f);
                e.values[i] = freq;
            }
        } else {
            // Sample
            is_sample_pattern_ = true;
            std::uint32_t sample_id = 0;
            // Always collect sample name for runtime resolution
            if (!atom_data.sample_name.empty()) {
                sample_names_.insert(atom_data.sample_name);
                // Assign type_id for per-type routing (e.g., match e.type { 1: kick, 2: snare })
                e.type_id = get_or_assign_type_id(atom_data.sample_name);
                // Record mapping for deferred resolution in WASM
                // Use current event count as index (before adding)
                std::uint16_t event_idx = static_cast<std::uint16_t>(
                    seq_idx < sequence_events_.size() ? sequence_events_[seq_idx].size() : 0);
                sample_mappings_.push_back(SequenceSampleMapping{
                    seq_idx,
                    event_idx,
                    atom_data.sample_name,
                    atom_data.sample_bank,
                    atom_data.sample_variant
                });
            }
            if (sample_registry_ && !atom_data.sample_name.empty()) {
                sample_id = sample_registry_->get_id(atom_data.sample_name);
            }
            e.values[0] = static_cast<float>(sample_id);
        }

        add_event_to_sequence(seq_idx, e);
    }

    // MiniGroup [a b c]: sequential concatenation, subdivide time
    void compile_group_events(const Node& n, std::uint16_t seq_idx,
                               float time_offset, float time_span) {
        // Same logic as compile_pattern_node
        std::vector<NodeIndex> children;
        std::vector<float> weights;
        float total_weight = 0.0f;

        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            float weight = get_node_weight(child);
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                children.push_back(child);
                weights.push_back(weight);
                total_weight += weight;
            }
            child = arena_[child].next_sibling;
        }

        if (children.empty()) return;
        if (total_weight <= 0.0f) total_weight = static_cast<float>(children.size());

        float accumulated_time = 0.0f;
        for (std::size_t i = 0; i < children.size(); ++i) {
            float child_span = (weights[i] / total_weight) * time_span;
            float child_offset = time_offset + accumulated_time;
            compile_into_sequence(children[i], seq_idx, child_offset, child_span);
            accumulated_time += child_span;
        }
    }

    // Create a new sub-sequence and return its index
    std::uint16_t create_sub_sequence(cedar::SequenceMode mode) {
        cedar::Sequence new_seq;
        new_seq.mode = mode;
        new_seq.duration = 1.0f;
        new_seq.events = nullptr;  // Will be set in finalize_sequences
        new_seq.num_events = 0;
        new_seq.capacity = 0;

        std::uint16_t new_idx = static_cast<std::uint16_t>(sequences_.size());
        sequences_.push_back(new_seq);
        sequence_events_.push_back({});  // Add empty event vector
        return new_idx;
    }

    // MiniSequence <a b c>: ALTERNATE mode (one per call, cycles through)
    void compile_alternate_sequence(const Node& n, std::uint16_t parent_seq_idx,
                                     float time_offset, float time_span) {
        // Create a sub-sequence with ALTERNATE mode
        std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::ALTERNATE);

        // Add each child as an event in the alternate sequence
        // Compound children (groups, patterns, etc.) are wrapped in NORMAL sub-sequences
        // so <[a b] [c d]> alternates between groups, not individual elements
        // Support !N repeat modifier: <a!3 b> becomes 4 choices (a, a, a, b)
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                compile_alternate_child(child, new_seq_idx);
            }
            child = arena_[child].next_sibling;
        }

        if (sequence_events_[new_seq_idx].empty()) return;

        // Add a SUB_SEQ event pointing to it
        cedar::Event e;
        e.type = cedar::EventType::SUB_SEQ;
        e.time = time_offset;
        e.duration = time_span;
        e.chance = 1.0f;
        e.seq_id = new_seq_idx;
        add_event_to_sequence(parent_seq_idx, e);
    }

    // MiniChoice a | b | c: RANDOM mode (pick one randomly)
    void compile_choice_sequence(const Node& n, std::uint16_t parent_seq_idx,
                                  float time_offset, float time_span) {
        // Create a sub-sequence with RANDOM mode
        std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::RANDOM);

        // Add each child as an event in the random sequence
        // Compound children (groups, patterns, etc.) are wrapped in NORMAL sub-sequences
        // so [a b] | [c d] picks between groups, not individual elements
        // Support !N repeat modifier: a!3 | b becomes 4 choices (a, a, a, b)
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            int repeat = get_node_repeat(child);
            for (int i = 0; i < repeat; ++i) {
                compile_alternate_child(child, new_seq_idx);
            }
            child = arena_[child].next_sibling;
        }

        if (sequence_events_[new_seq_idx].empty()) return;

        // Add a SUB_SEQ event pointing to it
        cedar::Event e;
        e.type = cedar::EventType::SUB_SEQ;
        e.time = time_offset;
        e.duration = time_span;
        e.chance = 1.0f;
        e.seq_id = new_seq_idx;
        add_event_to_sequence(parent_seq_idx, e);
    }

    // MiniPolyrhythm [a, b, c]: all elements simultaneously
    void compile_polyrhythm_events(const Node& n, std::uint16_t seq_idx,
                                    float time_offset, float time_span) {
        // Each child occupies the same time span
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            compile_into_sequence(child, seq_idx, time_offset, time_span);
            child = arena_[child].next_sibling;
        }
    }

    // MiniEuclidean: Euclidean rhythm pattern
    void compile_euclidean_events(const Node& n, std::uint16_t seq_idx,
                                   float time_offset, float time_span) {
        const auto& euclid_data = n.as_mini_euclidean();
        std::uint32_t hits = euclid_data.hits;
        std::uint32_t steps = euclid_data.steps;
        std::uint32_t rotation = euclid_data.rotation;

        if (steps == 0 || hits == 0) return;

        // Generate Euclidean pattern
        std::uint32_t pattern = compute_euclidean_pattern(hits, steps, rotation);

        // Child element to place on hits
        NodeIndex child = n.first_child;

        float step_span = time_span / static_cast<float>(steps);
        for (std::uint32_t i = 0; i < steps; ++i) {
            if ((pattern >> i) & 1) {
                float step_offset = time_offset + static_cast<float>(i) * step_span;
                if (child != NULL_NODE) {
                    compile_into_sequence(child, seq_idx, step_offset, step_span);
                }
            }
        }
    }

    // Compute Euclidean pattern as bitmask
    std::uint32_t compute_euclidean_pattern(std::uint32_t hits, std::uint32_t steps,
                                             std::uint32_t rotation) {
        if (steps == 0 || hits == 0) return 0;
        if (hits >= steps) return (1u << steps) - 1;

        std::uint32_t pattern = 0;
        float bucket = 0.0f;
        float increment = static_cast<float>(hits) / static_cast<float>(steps);

        for (std::uint32_t i = 0; i < steps; ++i) {
            bucket += increment;
            if (bucket >= 1.0f) {
                pattern |= (1u << i);
                bucket -= 1.0f;
            }
        }

        // Apply rotation
        if (rotation > 0 && steps > 0) {
            rotation = rotation % steps;
            std::uint32_t mask = (1u << steps) - 1;
            pattern = ((pattern >> rotation) | (pattern << (steps - rotation))) & mask;
        }

        return pattern;
    }

    // MiniModified: handle modifiers (*n, !n, ?n, @n)
    void compile_modified_node(const Node& n, std::uint16_t seq_idx,
                                float time_offset, float time_span) {
        const auto& mod_data = n.as_mini_modifier();
        NodeIndex child = n.first_child;

        if (child == NULL_NODE) return;

        switch (mod_data.modifier_type) {
            case Node::MiniModifierType::Speed: {
                // *N: Speed up - creates N events (for alternates) or compresses time
                int count = static_cast<int>(mod_data.value);
                if (count <= 0) count = 1;

                // Check if child is MiniSequence (alternate)
                const Node& child_node = arena_[child];
                if (child_node.type == NodeType::MiniSequence) {
                    // <a b c>*8 -> 8 SUB_SEQ events pointing to ALTERNATE sequence
                    std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::ALTERNATE);

                    // Track sequence index for sample mappings
                    std::uint16_t saved_seq_idx = current_seq_idx_;
                    current_seq_idx_ = new_seq_idx;

                    NodeIndex alt_child = child_node.first_child;
                    while (alt_child != NULL_NODE) {
                        compile_into_sequence(alt_child, new_seq_idx, 0.0f, 1.0f);
                        alt_child = arena_[alt_child].next_sibling;
                    }

                    current_seq_idx_ = saved_seq_idx;

                    if (!sequence_events_[new_seq_idx].empty()) {
                        // Create N SUB_SEQ events
                        float event_span = time_span / static_cast<float>(count);
                        for (int i = 0; i < count; ++i) {
                            cedar::Event e;
                            e.type = cedar::EventType::SUB_SEQ;
                            e.time = time_offset + static_cast<float>(i) * event_span;
                            e.duration = event_span;
                            e.chance = 1.0f;
                            e.seq_id = new_seq_idx;
                            add_event_to_sequence(seq_idx, e);
                        }
                    }
                } else {
                    // Regular speed modifier - wrap N fast events in a sub-sequence
                    // so they form ONE element (not N separate elements)
                    std::uint16_t new_seq_idx = create_sub_sequence(cedar::SequenceMode::NORMAL);

                    // Track sequence index for sample mappings
                    std::uint16_t saved_seq_idx = current_seq_idx_;
                    current_seq_idx_ = new_seq_idx;

                    float event_span = 1.0f / static_cast<float>(count);
                    for (int i = 0; i < count; ++i) {
                        float event_offset = static_cast<float>(i) * event_span;
                        compile_into_sequence(child, new_seq_idx, event_offset, event_span);
                    }

                    current_seq_idx_ = saved_seq_idx;

                    if (!sequence_events_[new_seq_idx].empty()) {
                        cedar::Event e;
                        e.type = cedar::EventType::SUB_SEQ;
                        e.time = time_offset;
                        e.duration = time_span;
                        e.chance = 1.0f;
                        e.seq_id = new_seq_idx;
                        add_event_to_sequence(seq_idx, e);
                    }
                }
                break;
            }

            case Node::MiniModifierType::Repeat: {
                // !N: Handled by parent enumeration via get_node_repeat()
                // Just compile the child once with full time span
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
            }

            case Node::MiniModifierType::Chance: {
                // ?N: Chance modifier - wrap in a sequence that applies chance
                // For simplicity, we compile the child and then modify the last event's chance
                std::size_t events_before = sequence_events_[seq_idx].size();
                compile_into_sequence(child, seq_idx, time_offset, time_span);

                // Apply chance to all new events
                float chance = mod_data.value;
                auto& events = sequence_events_[seq_idx];
                for (std::size_t i = events_before; i < events.size(); ++i) {
                    events[i].chance = chance;
                }
                break;
            }

            case Node::MiniModifierType::Slow: {
                // /N: Slow down - just compile with same span (handled at cycle level)
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
            }

            case Node::MiniModifierType::Weight:
                // Weight is handled by parent (get_node_weight)
                compile_into_sequence(child, seq_idx, time_offset, time_span);
                break;
        }
    }

    // Get the weight (@N) of a node (default 1.0)
    float get_node_weight(NodeIndex node_idx) {
        const Node& n = arena_[node_idx];
        if (n.type == NodeType::MiniModified) {
            const auto& mod = n.as_mini_modifier();
            if (mod.modifier_type == Node::MiniModifierType::Weight) {
                return mod.value;
            }
        }
        return 1.0f;
    }

    // Get the repeat count (!N) of a node (default 1)
    int get_node_repeat(NodeIndex node_idx) {
        const Node& n = arena_[node_idx];
        if (n.type == NodeType::MiniModified) {
            const auto& mod = n.as_mini_modifier();
            if (mod.modifier_type == Node::MiniModifierType::Repeat) {
                return static_cast<int>(mod.value);
            }
        }
        return 1;
    }

    // Get or assign a type_id for a sample name (for per-type routing)
    std::uint16_t get_or_assign_type_id(const std::string& sample_name) {
        auto it = sample_type_ids_.find(sample_name);
        if (it != sample_type_ids_.end()) {
            return it->second;
        }
        // Assign new type_id (starting from 1, 0 = no type)
        std::uint16_t type_id = next_type_id_++;
        sample_type_ids_[sample_name] = type_id;
        return type_id;
    }

    const AstArena& arena_;
    SampleRegistry* sample_registry_ = nullptr;
    std::vector<cedar::Sequence> sequences_;
    std::vector<std::vector<cedar::Event>> sequence_events_;  // Event storage for each sequence
    std::set<std::string> sample_names_;
    std::vector<SequenceSampleMapping> sample_mappings_;
    std::unordered_map<std::string, std::uint16_t> sample_type_ids_;  // Sample name → type_id
    std::uint16_t next_type_id_ = 1;  // Start at 1, 0 = no type (pitch patterns)
    bool is_sample_pattern_ = false;
    std::uint32_t pattern_base_offset_ = 0;
    std::uint16_t current_seq_idx_ = 0;  // Track current sequence index for sample mappings
    std::uint32_t total_events_ = 0;     // Total event count across all sequences
};

// ============================================================================
// End Compilers
// ============================================================================

// Handle MiniLiteral (pattern) nodes
std::uint16_t CodeGenerator::handle_mini_literal(NodeIndex node, const Node& n) {
    NodeIndex pattern_node = n.first_child;
    NodeIndex closure_node = NULL_NODE;

    if (pattern_node != NULL_NODE) {
        closure_node = ast_->arena[pattern_node].next_sibling;
    }

    if (pattern_node == NULL_NODE) {
        error("E114", "Pattern has no parsed content", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint32_t pat_count = call_counters_["pat"]++;
    push_path("pat#" + std::to_string(pat_count));
    std::uint32_t state_id = compute_state_id();

    // Use the SequenceCompiler for lazy queryable patterns
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    // Set base offset so event source_offset values are pattern-relative
    const Node& pattern = ast_->arena[pattern_node];
    compiler.set_pattern_base_offset(pattern.location.offset);
    if (!compiler.compile(pattern_node)) {
        // Empty pattern - emit zero
        std::uint16_t out = emit_zero(buffers_, instructions_);
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        pop_path();
        node_buffers_[node] = out;
        return out;
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // Determine cycle length from top-level element count (each element = 1 beat)
    std::uint32_t num_elements = compiler.count_top_level_elements(pattern_node);
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SEQPAT_QUERY instruction (queries pattern at block boundaries)
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;  // No direct output
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Check for polyphonic patterns (chords with multiple values per event)
    std::uint8_t max_voices = compiler.max_voices();

    // Emit per-voice SEQPAT_STEP/GATE/TYPE and register polyphonic fields
    if (!emit_per_voice_seqpat(node, state_id, max_voices, value_buf, velocity_buf,
                                trigger_buf, is_sample_pattern, n.location)) {
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = compiler.sequence_events();  // Store event vectors
    seq_init.total_events = compiler.total_events();        // Size hint for arena allocation
    seq_init.is_sample_pattern = is_sample_pattern;
    seq_init.pattern_location = pattern.location;  // Store pattern content location for UI
    seq_init.sequence_sample_mappings = compiler.sample_mappings();  // For deferred sample ID resolution
    seq_init.ast_json = serialize_mini_ast_json(pattern_node, ast_->arena);  // Serialize AST for debug UI
    state_inits_.push_back(std::move(seq_init));

    std::uint16_t result_buf = value_buf;

    // Handle sample patterns - need to wire to SAMPLE_PLAY
    if (is_sample_pattern) {
        std::uint16_t pitch_buf = buffers_.allocate();
        std::uint16_t output_buf = buffers_.allocate();

        if (pitch_buf == BufferAllocator::BUFFER_UNUSED ||
            output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return value_buf;  // Return value buffer as fallback
        }

        // Set pitch to 1.0 for sample playback
        cedar::Instruction pitch_inst{};
        pitch_inst.opcode = cedar::Opcode::PUSH_CONST;
        pitch_inst.out_buffer = pitch_buf;
        pitch_inst.inputs[0] = 0xFFFF;
        pitch_inst.inputs[1] = 0xFFFF;
        pitch_inst.inputs[2] = 0xFFFF;
        pitch_inst.inputs[3] = 0xFFFF;
        encode_const_value(pitch_inst, 1.0f);
        emit(pitch_inst);

        // Wire up sample player
        cedar::Instruction sample_inst{};
        sample_inst.opcode = cedar::Opcode::SAMPLE_PLAY;
        sample_inst.out_buffer = output_buf;
        sample_inst.inputs[0] = trigger_buf;   // trigger
        sample_inst.inputs[1] = pitch_buf;     // pitch
        sample_inst.inputs[2] = value_buf;     // sample_id
        sample_inst.inputs[3] = 0xFFFF;
        sample_inst.state_id = state_id + 1;
        emit(sample_inst);

        result_buf = output_buf;
    } else if (closure_node != NULL_NODE) {
        // Handle closure for pitch patterns
        const Node& closure = ast_->arena[closure_node];
        std::vector<std::string> param_names;
        NodeIndex child = closure.first_child;
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

        if (param_names.size() >= 1) symbols_->define_variable(param_names[0], trigger_buf);
        if (param_names.size() >= 2) symbols_->define_variable(param_names[1], velocity_buf);
        if (param_names.size() >= 3) symbols_->define_variable(param_names[2], value_buf);

        if (body != NULL_NODE) {
            result_buf = visit(body);
        }
    }

    pop_path();
    node_buffers_[node] = result_buf;

    // If closure is present, clear polyphonic registration since the user
    // processes pattern events manually through closure parameters
    if (closure_node != NULL_NODE && max_voices > 1) {
        polyphonic_fields_.erase(node);
        multi_buffers_.erase(node);
    }

    return result_buf;
}

// Resolve polyphonic field name to buffer vector
bool CodeGenerator::resolve_polyphonic_field(const PolyphonicFields& poly,
                                             const std::string& field_name,
                                             std::vector<std::uint16_t>& out_buffers) const {
    if (field_name == "freq" || field_name == "f" || field_name == "pitch") {
        out_buffers = poly.freq_buffers;
    } else if (field_name == "vel" || field_name == "v" || field_name == "velocity") {
        out_buffers = poly.vel_buffers;
    } else if (field_name == "trig" || field_name == "t" || field_name == "trigger") {
        out_buffers = poly.trig_buffers;
    } else if (field_name == "gate" || field_name == "g") {
        out_buffers = poly.gate_buffers;
    } else if (field_name == "type") {
        out_buffers = poly.type_buffers;
    } else {
        return false;
    }
    return true;
}

// Emit per-voice SEQPAT_STEP/GATE/TYPE and register polyphonic fields + multi-buffers
bool CodeGenerator::emit_per_voice_seqpat(NodeIndex node, std::uint32_t state_id,
                                           std::uint8_t max_voices,
                                           std::uint16_t value_buf, std::uint16_t velocity_buf,
                                           std::uint16_t trigger_buf,
                                           bool is_sample_pattern, SourceLocation loc,
                                           std::uint16_t clock_override) {
    std::vector<std::uint16_t> voice_freq_buffers;
    std::vector<std::uint16_t> voice_vel_buffers;
    std::vector<std::uint16_t> voice_trig_buffers;
    std::vector<std::uint16_t> voice_gate_buffers;
    std::vector<std::uint16_t> voice_type_buffers;

    for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
        std::uint16_t voice_value_buf = (voice == 0) ? value_buf : buffers_.allocate();
        std::uint16_t voice_vel_buf = (voice == 0) ? velocity_buf : buffers_.allocate();
        std::uint16_t voice_trig_buf = (voice == 0) ? trigger_buf : buffers_.allocate();
        std::uint16_t voice_gate_buf = buffers_.allocate();
        std::uint16_t voice_type_buf = buffers_.allocate();

        if (voice_value_buf == BufferAllocator::BUFFER_UNUSED ||
            voice_vel_buf == BufferAllocator::BUFFER_UNUSED ||
            voice_trig_buf == BufferAllocator::BUFFER_UNUSED ||
            voice_gate_buf == BufferAllocator::BUFFER_UNUSED ||
            voice_type_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            return false;
        }

        cedar::Instruction step_inst{};
        step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
        step_inst.out_buffer = voice_value_buf;
        step_inst.inputs[0] = voice_vel_buf;
        step_inst.inputs[1] = voice_trig_buf;
        step_inst.inputs[2] = voice;
        step_inst.inputs[3] = clock_override;
        step_inst.inputs[4] = 0xFFFF;
        step_inst.state_id = state_id;
        emit(step_inst);

        cedar::Instruction gate_inst{};
        gate_inst.opcode = cedar::Opcode::SEQPAT_GATE;
        gate_inst.out_buffer = voice_gate_buf;
        gate_inst.inputs[0] = voice;
        gate_inst.inputs[1] = clock_override;
        gate_inst.inputs[2] = 0xFFFF;
        gate_inst.inputs[3] = 0xFFFF;
        gate_inst.inputs[4] = 0xFFFF;
        gate_inst.state_id = state_id;
        emit(gate_inst);

        cedar::Instruction type_inst{};
        type_inst.opcode = cedar::Opcode::SEQPAT_TYPE;
        type_inst.out_buffer = voice_type_buf;
        type_inst.inputs[0] = voice;
        type_inst.inputs[1] = clock_override;
        type_inst.inputs[2] = 0xFFFF;
        type_inst.inputs[3] = 0xFFFF;
        type_inst.inputs[4] = 0xFFFF;
        type_inst.state_id = state_id;
        emit(type_inst);

        voice_freq_buffers.push_back(voice_value_buf);
        voice_vel_buffers.push_back(voice_vel_buf);
        voice_trig_buffers.push_back(voice_trig_buf);
        voice_gate_buffers.push_back(voice_gate_buf);
        voice_type_buffers.push_back(voice_type_buf);
    }

    // Store monophonic record fields (voice 0)
    std::unordered_map<std::string, std::uint16_t> pattern_fields;
    pattern_fields["freq"] = value_buf;
    pattern_fields["vel"] = velocity_buf;
    pattern_fields["trig"] = trigger_buf;
    pattern_fields["gate"] = voice_gate_buffers.empty() ? BufferAllocator::BUFFER_UNUSED : voice_gate_buffers[0];
    record_fields_[node] = std::move(pattern_fields);

    // Register polyphonic fields and multi-buffer for polyphonic patterns
    if (max_voices > 1 && !is_sample_pattern) {
        PolyphonicFields poly_fields;
        poly_fields.freq_buffers = voice_freq_buffers;
        poly_fields.vel_buffers = voice_vel_buffers;
        poly_fields.trig_buffers = voice_trig_buffers;
        poly_fields.gate_buffers = voice_gate_buffers;
        poly_fields.type_buffers = voice_type_buffers;
        poly_fields.num_voices = max_voices;
        polyphonic_fields_[node] = std::move(poly_fields);

        register_multi_buffer(node, std::move(voice_freq_buffers));
    }

    return true;
}

// Handle pattern variable reference
std::uint16_t CodeGenerator::handle_pattern_reference(const std::string& name,
                                                       NodeIndex pattern_node,
                                                       SourceLocation loc) {
    if (pattern_node == NULL_NODE) {
        error("E123", "Pattern variable '" + name + "' has invalid pattern node", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pattern_n = ast_->arena[pattern_node];
    if (pattern_n.type != NodeType::MiniLiteral) {
        error("E124", "Pattern variable '" + name + "' does not refer to a pattern", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    push_path(name);
    std::uint32_t state_id = compute_state_id();

    NodeIndex mini_pattern = pattern_n.first_child;
    if (mini_pattern == NULL_NODE) {
        error("E114", "Pattern has no parsed content", loc);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Use the SequenceCompiler
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    if (!compiler.compile(mini_pattern)) {
        // Empty pattern - emit zero
        std::uint16_t out = emit_zero(buffers_, instructions_);
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
        }
        pop_path();
        return out;
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // Determine cycle length from top-level element count (each element = 1 beat)
    std::uint32_t num_elements = compiler.count_top_level_elements(mini_pattern);
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", loc);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SEQPAT_QUERY
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit per-voice SEQPAT_STEP/GATE/TYPE (polyphonic if pattern has chords)
    std::uint8_t max_voices = compiler.max_voices();
    if (!emit_per_voice_seqpat(pattern_node, state_id, max_voices, value_buf, velocity_buf,
                                trigger_buf, is_sample_pattern, loc)) {
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Store sequence program
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = compiler.sequence_events();  // Store event vectors
    seq_init.total_events = compiler.total_events();        // Size hint for arena allocation
    seq_init.is_sample_pattern = is_sample_pattern;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();  // For deferred sample ID resolution
    state_inits_.push_back(std::move(seq_init));

    pop_path();
    return value_buf;
}

// Handle chord() calls - uses SEQPAT system via SequenceCompiler
std::uint16_t CodeGenerator::handle_chord_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E125", "chord() requires exactly 1 argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& arg_node = ast_->arena[arg];
    NodeIndex str_node = (arg_node.type == NodeType::Argument) ? arg_node.first_child : arg;

    if (str_node == NULL_NODE) {
        error("E125", "chord() requires a string argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& str_n = ast_->arena[str_node];
    if (str_n.type != NodeType::StringLit) {
        error("E126", "chord() argument must be a string literal (e.g., \"Am\", \"C7 F G\")",
              str_n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string chord_str = str_n.as_string();

    // Parse using mini-notation parser with sample_only=false (default)
    // This enables chord symbol recognition (Am, C7, Fmaj7, etc.)
    auto [pattern_root, diags] = parse_mini(chord_str, const_cast<AstArena&>(ast_->arena),
                                            str_n.location, /*sample_only=*/false);

    // Report any parse errors
    for (const auto& diag : diags) {
        if (diag.severity == Severity::Error) {
            diagnostics_.push_back(diag);
        }
    }

    if (pattern_root == NULL_NODE) {
        error("E127", "Failed to parse chord pattern: \"" + chord_str + "\"", str_n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Use SequenceCompiler to compile the chord pattern (same as pat())
    std::uint32_t chord_count = call_counters_["chord"]++;
    push_path("chord#" + std::to_string(chord_count));
    std::uint32_t state_id = compute_state_id();

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    const Node& pattern = ast_->arena[pattern_root];
    compiler.set_pattern_base_offset(pattern.location.offset);

    if (!compiler.compile(pattern_root)) {
        error("E127", "Failed to compile chord pattern: \"" + chord_str + "\"", str_n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Determine cycle length from top-level element count
    std::uint32_t num_elements = compiler.count_top_level_elements(pattern_root);
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SEQPAT_QUERY instruction
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Check for polyphonic patterns (chords with multiple values per event)
    std::uint8_t max_voices = compiler.max_voices();
    std::vector<std::uint16_t> voice_freq_buffers;
    std::vector<std::uint16_t> voice_vel_buffers;
    std::vector<std::uint16_t> voice_trig_buffers;

    // Emit SEQPAT_STEP for each voice
    // For polyphonic patterns, each voice gets its own freq, vel, and trig buffers
    for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
        std::uint16_t voice_value_buf = (voice == 0) ? value_buf : buffers_.allocate();
        std::uint16_t voice_vel_buf = (voice == 0) ? velocity_buf : buffers_.allocate();
        std::uint16_t voice_trig_buf = (voice == 0) ? trigger_buf : buffers_.allocate();

        if (voice_value_buf == BufferAllocator::BUFFER_UNUSED ||
            voice_vel_buf == BufferAllocator::BUFFER_UNUSED ||
            voice_trig_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction step_inst{};
        step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
        step_inst.out_buffer = voice_value_buf;
        step_inst.inputs[0] = voice_vel_buf;
        step_inst.inputs[1] = voice_trig_buf;
        // Voice index for polyphonic selection
        step_inst.inputs[2] = voice;
        step_inst.inputs[3] = 0xFFFF;
        step_inst.inputs[4] = 0xFFFF;
        step_inst.state_id = state_id;
        emit(step_inst);

        voice_freq_buffers.push_back(voice_value_buf);
        voice_vel_buffers.push_back(voice_vel_buf);
        voice_trig_buffers.push_back(voice_trig_buf);
    }

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = compiler.sequence_events();
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = false;
    seq_init.pattern_location = str_n.location;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits_.push_back(std::move(seq_init));

    pop_path();

    // Store polyphonic field buffers for %.field access with multi-buffer support
    if (max_voices > 1) {
        PolyphonicFields poly_fields;
        poly_fields.freq_buffers = voice_freq_buffers;
        poly_fields.vel_buffers = voice_vel_buffers;
        poly_fields.trig_buffers = voice_trig_buffers;
        poly_fields.num_voices = max_voices;
        polyphonic_fields_[node] = std::move(poly_fields);
    }

    // Register multi-buffer for polyphony (freq buffers are the main output)
    std::uint16_t first_buf = register_multi_buffer(node, std::move(voice_freq_buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

// ============================================================================
// Pattern transformation handlers
// ============================================================================

// Helper: Get pattern argument from a function call
// Returns the MiniLiteral node or NULL_NODE if not a valid pattern
static NodeIndex get_pattern_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = n.first_child;
    std::size_t idx = 0;
    while (arg != NULL_NODE && idx < arg_index) {
        arg = ast.arena[arg].next_sibling;
        idx++;
    }
    if (arg == NULL_NODE) return NULL_NODE;

    // Unwrap Argument node if present
    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::Argument) {
        arg = arg_node.first_child;
    }

    return arg;
}

// Helper: Get numeric argument from a function call
// Returns the value or default if not a valid number
static std::optional<float> get_number_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = get_pattern_arg(ast, n, arg_index);
    if (arg == NULL_NODE) return std::nullopt;

    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::NumberLit) {
        return static_cast<float>(arg_node.as_number());
    }
    return std::nullopt;
}

// Helper: Get string argument from a function call
// Returns the string value or nullopt if not a valid string
static std::optional<std::string> get_string_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = get_pattern_arg(ast, n, arg_index);
    if (arg == NULL_NODE) return std::nullopt;

    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::StringLit) {
        return arg_node.as_string();
    }
    return std::nullopt;
}

// Helper: Check if a node is a pattern-producing expression
// Returns true for MiniLiteral or Call to pat/seq/timeline
static bool is_pattern_expr(const Ast& ast, NodeIndex node) {
    if (node == NULL_NODE) return false;

    const Node& n = ast.arena[node];

    if (n.type == NodeType::MiniLiteral) return true;

    if (n.type == NodeType::Call) {
        const std::string& func_name = n.as_identifier();
        return func_name == "pat" || func_name == "timeline" ||
               func_name == "slow" || func_name == "fast" ||
               func_name == "rev" || func_name == "transpose" || func_name == "velocity" ||
               func_name == "bank" || func_name == "n" || func_name == "transport";
    }

    return false;
}

// Helper: Compile a pattern and return the compiled data
// Returns true on success. On success, populates out_* parameters.
static bool compile_pattern_for_transform(
    CodeGenerator& gen,
    const Ast& ast,
    NodeIndex pattern_arg,
    SampleRegistry* sample_registry,
    SequenceCompiler& compiler,
    NodeIndex& out_pattern_node,
    std::uint32_t& out_num_elements) {

    const Node& pat_node = ast.arena[pattern_arg];

    // Handle MiniLiteral: get the pattern node inside
    if (pat_node.type == NodeType::MiniLiteral) {
        out_pattern_node = pat_node.first_child;
        if (out_pattern_node == NULL_NODE) {
            return false;
        }

        const Node& pattern = ast.arena[out_pattern_node];
        compiler.set_pattern_base_offset(pattern.location.offset);

        if (!compiler.compile(out_pattern_node)) {
            return false;
        }

        out_num_elements = compiler.count_top_level_elements(out_pattern_node);
        return true;
    }

    // Handle pattern-producing Call nodes (pat, seq, etc.)
    // For now, recurse and compile the Call to get the inner pattern
    if (pat_node.type == NodeType::Call) {
        // Get the first argument which should be a string literal
        NodeIndex first_arg = pat_node.first_child;
        if (first_arg != NULL_NODE) {
            const Node& arg_node = ast.arena[first_arg];
            NodeIndex actual_arg = first_arg;

            // Unwrap Argument node
            if (arg_node.type == NodeType::Argument) {
                actual_arg = arg_node.first_child;
            }

            if (actual_arg != NULL_NODE) {
                const Node& actual_node = ast.arena[actual_arg];
                if (actual_node.type == NodeType::MiniLiteral) {
                    out_pattern_node = actual_node.first_child;
                    if (out_pattern_node != NULL_NODE) {
                        const Node& pattern = ast.arena[out_pattern_node];
                        compiler.set_pattern_base_offset(pattern.location.offset);

                        if (compiler.compile(out_pattern_node)) {
                            out_num_elements = compiler.count_top_level_elements(out_pattern_node);
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

// Helper: Emit a compiled pattern with transformations applied
// This is the common code for emitting SEQPAT_QUERY/SEQPAT_STEP
static std::uint16_t emit_pattern_with_state(
    CodeGenerator& gen,
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions,
    std::vector<StateInitData>& state_inits,
    std::set<std::string>& required_samples,
    std::unordered_map<NodeIndex, std::uint16_t>& node_buffers,
    std::unordered_map<NodeIndex, std::unordered_map<std::string, std::uint16_t>>& record_fields,
    NodeIndex node,
    std::uint32_t state_id,
    float cycle_length,
    const SequenceCompiler& compiler,
    std::vector<std::vector<cedar::Event>>& sequence_events,
    const SourceLocation& pattern_loc,
    const SourceLocation& call_loc,
    void (*emit_fn)(std::vector<cedar::Instruction>&, const cedar::Instruction&)) {

    // Collect required samples
    compiler.collect_samples(required_samples);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers.allocate();
    std::uint16_t velocity_buf = buffers.allocate();
    std::uint16_t trigger_buf = buffers.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SEQPAT_QUERY instruction
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit_fn(instructions, query_inst);

    // Check for polyphonic patterns
    std::uint8_t max_voices = compiler.max_voices();
    std::vector<std::uint16_t> voice_buffers;

    // Emit SEQPAT_STEP for each voice
    for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
        std::uint16_t voice_value_buf = (voice == 0) ? value_buf : buffers.allocate();
        if (voice_value_buf == BufferAllocator::BUFFER_UNUSED) {
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction step_inst{};
        step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
        step_inst.out_buffer = voice_value_buf;
        step_inst.inputs[0] = (voice == 0) ? velocity_buf : 0xFFFF;
        step_inst.inputs[1] = (voice == 0) ? trigger_buf : 0xFFFF;
        step_inst.inputs[2] = voice;
        step_inst.inputs[3] = 0xFFFF;
        step_inst.inputs[4] = 0xFFFF;
        step_inst.state_id = state_id;
        emit_fn(instructions, step_inst);

        voice_buffers.push_back(voice_value_buf);
    }

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    seq_init.pattern_location = pattern_loc;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits.push_back(std::move(seq_init));

    // Store pattern field buffers for %.field access
    std::unordered_map<std::string, std::uint16_t> pattern_fields;
    pattern_fields["freq"] = value_buf;
    pattern_fields["vel"] = velocity_buf;
    pattern_fields["trig"] = trigger_buf;
    record_fields[node] = std::move(pattern_fields);

    node_buffers[node] = value_buf;
    return value_buf;
}

// Helper to emit instruction (wrapper for member function access)
static void emit_instruction_helper(std::vector<cedar::Instruction>& instructions,
                                    const cedar::Instruction& inst) {
    instructions.push_back(inst);
}

std::uint16_t CodeGenerator::handle_slow_call(NodeIndex node, const Node& n) {
    // slow(pattern, factor) - stretch pattern by factor
    // Multiplies all event times, durations, and cycle_length by factor

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto factor = get_number_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "slow() requires a pattern as first argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (!factor.has_value() || *factor <= 0) {
        error("E131", "slow() requires a positive number as second argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];

    // Accept MiniLiteral or pattern-producing Call nodes
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "slow() first argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Compile the pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "slow() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state ID
    std::uint32_t slow_count = call_counters_["slow"]++;
    push_path("slow#" + std::to_string(slow_count));
    std::uint32_t state_id = compute_state_id();

    // Get compiled events and apply slow transformation
    auto sequence_events = compiler.sequence_events();
    float slow_factor = *factor;

    // Transform all events: multiply time and duration by factor
    for (auto& seq_events : sequence_events) {
        for (auto& event : seq_events) {
            event.time *= slow_factor;
            event.duration *= slow_factor;
        }
    }

    // Adjust cycle length
    float cycle_length = static_cast<float>(std::max(1u, num_elements)) * slow_factor;

    const Node& pattern = ast_->arena[pattern_node];
    std::uint16_t result = emit_pattern_with_state(
        *this, buffers_, instructions_, state_inits_, required_samples_,
        node_buffers_, record_fields_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location,
        emit_instruction_helper);

    pop_path();

    if (result == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
    }

    return result;
}

std::uint16_t CodeGenerator::handle_fast_call(NodeIndex node, const Node& n) {
    // fast(pattern, factor) - compress pattern by factor
    // Divides all event times, durations, and cycle_length by factor

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto factor = get_number_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "fast() requires a pattern as first argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (!factor.has_value() || *factor <= 0) {
        error("E131", "fast() requires a positive number as second argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];

    // Accept MiniLiteral or pattern-producing Call nodes
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "fast() first argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Compile the pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "fast() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state ID
    std::uint32_t fast_count = call_counters_["fast"]++;
    push_path("fast#" + std::to_string(fast_count));
    std::uint32_t state_id = compute_state_id();

    // Get compiled events and apply fast transformation
    auto sequence_events = compiler.sequence_events();
    float fast_factor = *factor;

    // Transform all events: divide time and duration by factor
    for (auto& seq_events : sequence_events) {
        for (auto& event : seq_events) {
            event.time /= fast_factor;
            event.duration /= fast_factor;
        }
    }

    // Adjust cycle length
    float cycle_length = static_cast<float>(std::max(1u, num_elements)) / fast_factor;

    const Node& pattern = ast_->arena[pattern_node];
    std::uint16_t result = emit_pattern_with_state(
        *this, buffers_, instructions_, state_inits_, required_samples_,
        node_buffers_, record_fields_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location,
        emit_instruction_helper);

    pop_path();

    if (result == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
    }

    return result;
}

std::uint16_t CodeGenerator::handle_rev_call(NodeIndex node, const Node& n) {
    // rev(pattern) - reverse event order in pattern
    // Reverses the start times: new_time = cycle_duration - old_time - old_duration

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);

    if (pattern_arg == NULL_NODE) {
        error("E130", "rev() requires a pattern as argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];

    // Accept MiniLiteral or pattern-producing Call nodes
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "rev() argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Compile the pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "rev() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state ID
    std::uint32_t rev_count = call_counters_["rev"]++;
    push_path("rev#" + std::to_string(rev_count));
    std::uint32_t state_id = compute_state_id();

    // Get compiled events and apply reverse transformation
    auto sequence_events = compiler.sequence_events();
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    // Transform all events: reverse time positions within each sequence
    // For events normalized to [0, 1), we use: new_time = 1 - old_time - old_duration
    // But since we work with beats, we use cycle_length
    for (auto& seq_events : sequence_events) {
        for (auto& event : seq_events) {
            // Calculate new start time so event ends where it used to start
            // old_end = old_time + old_duration
            // new_time = cycle_length - old_end = cycle_length - old_time - old_duration
            float new_time = cycle_length - event.time - event.duration;
            if (new_time < 0.0f) new_time = 0.0f;  // Clamp to avoid negative times
            event.time = new_time;
        }
    }

    const Node& pattern = ast_->arena[pattern_node];
    std::uint16_t result = emit_pattern_with_state(
        *this, buffers_, instructions_, state_inits_, required_samples_,
        node_buffers_, record_fields_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location,
        emit_instruction_helper);

    pop_path();

    if (result == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
    }

    return result;
}

std::uint16_t CodeGenerator::handle_transpose_call(NodeIndex node, const Node& n) {
    // transpose(pattern, semitones) - shift all pitches by semitones
    // Converts frequency to MIDI, adds semitones, converts back to frequency

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto semitones = get_number_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "transpose() requires a pattern as first argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (!semitones.has_value()) {
        error("E131", "transpose() requires a number as second argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];

    // Accept MiniLiteral or pattern-producing Call nodes
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "transpose() first argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Compile the pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "transpose() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state ID
    std::uint32_t transpose_count = call_counters_["transpose"]++;
    push_path("transpose#" + std::to_string(transpose_count));
    std::uint32_t state_id = compute_state_id();

    // Get compiled events and apply transpose transformation
    auto sequence_events = compiler.sequence_events();
    float semitones_value = *semitones;

    // Transpose ratio: multiply frequency by 2^(semitones/12)
    float transpose_ratio = std::pow(2.0f, semitones_value / 12.0f);

    // Transform all events: multiply frequency values by transpose ratio
    // Only for pitch patterns (not sample patterns)
    if (!compiler.is_sample_pattern()) {
        for (auto& seq_events : sequence_events) {
            for (auto& event : seq_events) {
                if (event.type == cedar::EventType::DATA) {
                    // Transpose all pitch values in the event
                    for (std::uint8_t i = 0; i < event.num_values; ++i) {
                        event.values[i] *= transpose_ratio;
                    }
                }
            }
        }
    }

    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    const Node& pattern = ast_->arena[pattern_node];
    std::uint16_t result = emit_pattern_with_state(
        *this, buffers_, instructions_, state_inits_, required_samples_,
        node_buffers_, record_fields_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location,
        emit_instruction_helper);

    pop_path();

    if (result == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
    }

    return result;
}

std::uint16_t CodeGenerator::handle_velocity_call(NodeIndex node, const Node& n) {
    // velocity(pattern, vel) - set velocity on all events
    // Note: This currently doesn't modify velocity since velocity is not stored
    // in cedar::Event. Instead, we'd need to emit instructions that multiply
    // the velocity buffer. For now, we emit a warning if vel != 1.0.

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto vel = get_number_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "velocity() requires a pattern as first argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (!vel.has_value() || *vel < 0 || *vel > 1) {
        error("E131", "velocity() requires a number between 0 and 1 as second argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];

    // Accept MiniLiteral or pattern-producing Call nodes
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "velocity() first argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Compile the pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "velocity() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state ID
    std::uint32_t velocity_count = call_counters_["velocity"]++;
    push_path("velocity#" + std::to_string(velocity_count));
    std::uint32_t state_id = compute_state_id();

    // Get compiled events
    auto sequence_events = compiler.sequence_events();
    float velocity_value = *vel;

    // Note: The current event structure doesn't store velocity per event.
    // Velocity is applied at the pattern level via the velocity buffer output.
    // For now, we compile the pattern normally and multiply the velocity output.
    // This is a simplified implementation - a full implementation would need
    // per-event velocity stored in the Event struct.

    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    // Collect required samples
    compiler.collect_samples(required_samples_);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SEQPAT_QUERY instruction
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit SEQPAT_STEP
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
    step_inst.out_buffer = value_buf;
    step_inst.inputs[0] = velocity_buf;
    step_inst.inputs[1] = trigger_buf;
    step_inst.inputs[2] = 0;
    step_inst.inputs[3] = 0xFFFF;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Now multiply the velocity buffer by the velocity value
    std::uint16_t vel_const_buf = buffers_.allocate();
    std::uint16_t scaled_velocity_buf = buffers_.allocate();

    if (vel_const_buf == BufferAllocator::BUFFER_UNUSED ||
        scaled_velocity_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Push the velocity constant
    cedar::Instruction vel_const{};
    vel_const.opcode = cedar::Opcode::PUSH_CONST;
    vel_const.out_buffer = vel_const_buf;
    vel_const.inputs[0] = 0xFFFF;
    vel_const.inputs[1] = 0xFFFF;
    vel_const.inputs[2] = 0xFFFF;
    vel_const.inputs[3] = 0xFFFF;
    codegen::encode_const_value(vel_const, velocity_value);
    emit(vel_const);

    // Multiply velocity by constant
    cedar::Instruction mul_inst{};
    mul_inst.opcode = cedar::Opcode::MUL;
    mul_inst.out_buffer = scaled_velocity_buf;
    mul_inst.inputs[0] = velocity_buf;
    mul_inst.inputs[1] = vel_const_buf;
    mul_inst.inputs[2] = 0xFFFF;
    mul_inst.inputs[3] = 0xFFFF;
    mul_inst.inputs[4] = 0xFFFF;
    emit(mul_inst);

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    const Node& pattern = ast_->arena[pattern_node];
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits_.push_back(std::move(seq_init));

    // Store pattern field buffers for %.field access
    // Use the scaled velocity buffer
    std::unordered_map<std::string, std::uint16_t> pattern_fields;
    pattern_fields["freq"] = value_buf;
    pattern_fields["vel"] = scaled_velocity_buf;  // Use scaled velocity
    pattern_fields["trig"] = trigger_buf;
    record_fields_[node] = std::move(pattern_fields);

    pop_path();
    node_buffers_[node] = value_buf;
    return value_buf;
}

std::uint16_t CodeGenerator::handle_bank_call(NodeIndex node, const Node& n) {
    // bank(pattern, bank_name) - set sample bank for all events
    // Sets the bank field on all sample mappings for deferred resolution

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto bank_name = get_string_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "bank() requires a pattern as first argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (!bank_name.has_value()) {
        error("E131", "bank() requires a string as second argument (e.g., \"TR808\")", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];

    // Accept MiniLiteral or pattern-producing Call nodes
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "bank() first argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Compile the pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "bank() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state ID
    std::uint32_t bank_count = call_counters_["bank"]++;
    push_path("bank#" + std::to_string(bank_count));
    std::uint32_t state_id = compute_state_id();

    // Get compiled events and sample mappings
    auto sequence_events = compiler.sequence_events();
    auto sample_mappings = compiler.sample_mappings();
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    // Update bank field on all sample mappings
    for (auto& mapping : sample_mappings) {
        mapping.bank = *bank_name;
    }

    // Collect required samples (with updated bank info)
    compiler.collect_samples(required_samples_);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SEQPAT_QUERY instruction
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit SEQPAT_STEP
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
    step_inst.out_buffer = value_buf;
    step_inst.inputs[0] = velocity_buf;
    step_inst.inputs[1] = trigger_buf;
    step_inst.inputs[2] = 0;
    step_inst.inputs[3] = 0xFFFF;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    const Node& pattern = ast_->arena[pattern_node];
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = std::move(sample_mappings);  // Use updated mappings
    state_inits_.push_back(std::move(seq_init));

    // Add to extended samples with bank info
    for (const auto& mapping : state_inits_.back().sequence_sample_mappings) {
        RequiredSample req_sample;
        req_sample.name = mapping.sample_name;
        req_sample.bank = mapping.bank;
        req_sample.variant = mapping.variant;

        std::string key = req_sample.key();
        if (required_samples_extended_keys_.find(key) == required_samples_extended_keys_.end()) {
            required_samples_extended_keys_.insert(key);
            required_samples_extended_.push_back(std::move(req_sample));
        }
    }

    // Store pattern field buffers for %.field access
    std::unordered_map<std::string, std::uint16_t> pattern_fields;
    pattern_fields["freq"] = value_buf;
    pattern_fields["vel"] = velocity_buf;
    pattern_fields["trig"] = trigger_buf;
    record_fields_[node] = std::move(pattern_fields);

    pop_path();
    node_buffers_[node] = value_buf;
    return value_buf;
}

std::uint16_t CodeGenerator::handle_n_call(NodeIndex node, const Node& n) {
    // n(pattern, variant) - set sample variant for all events
    // Two cases:
    //   1. n(pattern, number) - fixed variant for all events
    //   2. n(pattern, pattern) - variant per-event from another pattern (TODO: future)

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    NodeIndex variant_arg = get_pattern_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "n() requires a pattern as first argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    if (variant_arg == NULL_NODE) {
        error("E131", "n() requires a variant number or pattern as second argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];

    // Accept MiniLiteral or pattern-producing Call nodes
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "n() first argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Check variant argument type
    const Node& variant_node = ast_->arena[variant_arg];
    bool is_fixed_variant = (variant_node.type == NodeType::NumberLit);
    int fixed_variant = 0;

    if (is_fixed_variant) {
        fixed_variant = static_cast<int>(variant_node.as_number());
        if (fixed_variant < 0) {
            error("E131", "n() variant must be non-negative", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }
    } else if (variant_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, variant_arg)) {
        error("E131", "n() second argument must be a number or pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Compile the main pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "n() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state ID
    std::uint32_t n_count = call_counters_["n"]++;
    push_path("n#" + std::to_string(n_count));
    std::uint32_t state_id = compute_state_id();

    // Get compiled events and sample mappings
    auto sequence_events = compiler.sequence_events();
    auto sample_mappings = compiler.sample_mappings();
    float cycle_length = static_cast<float>(std::max(1u, num_elements));

    if (is_fixed_variant) {
        // Case 1: Fixed variant - set on all sample mappings
        for (auto& mapping : sample_mappings) {
            mapping.variant = static_cast<std::uint8_t>(fixed_variant);
        }
    } else {
        // Case 2: Per-event variant from pattern
        // Compile the variant pattern to get its events
        SequenceCompiler variant_compiler(ast_->arena, sample_registry_);
        NodeIndex variant_pattern_node = NULL_NODE;
        std::uint32_t variant_num_elements = 1;

        if (compile_pattern_for_transform(*this, *ast_, variant_arg, sample_registry_,
                                          variant_compiler, variant_pattern_node, variant_num_elements)) {
            // Get variant pattern events
            auto variant_events = variant_compiler.sequence_events();

            // Match events: for each sample event in the main pattern,
            // look up the corresponding variant value from the variant pattern.
            // If the variant pattern has fewer events, cycle through them.
            if (!variant_events.empty() && !variant_events[0].empty()) {
                std::size_t variant_idx = 0;
                std::size_t variant_count = variant_events[0].size();

                for (auto& mapping : sample_mappings) {
                    // Get the variant value from the variant pattern
                    const auto& variant_evt = variant_events[0][variant_idx % variant_count];
                    if (variant_evt.type == cedar::EventType::DATA && variant_evt.num_values > 0) {
                        // Use the first value as the variant index
                        int var_val = static_cast<int>(variant_evt.values[0]);
                        if (var_val >= 0) {
                            mapping.variant = static_cast<std::uint8_t>(var_val);
                        }
                    }
                    variant_idx++;
                }
            }
        }
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SEQPAT_QUERY instruction
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = 0xFFFF;
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = state_id;
    emit(query_inst);

    // Emit SEQPAT_STEP
    cedar::Instruction step_inst{};
    step_inst.opcode = cedar::Opcode::SEQPAT_STEP;
    step_inst.out_buffer = value_buf;
    step_inst.inputs[0] = velocity_buf;
    step_inst.inputs[1] = trigger_buf;
    step_inst.inputs[2] = 0;
    step_inst.inputs[3] = 0xFFFF;
    step_inst.inputs[4] = 0xFFFF;
    step_inst.state_id = state_id;
    emit(step_inst);

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    const Node& pattern = ast_->arena[pattern_node];
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = std::move(sample_mappings);  // Use updated mappings
    state_inits_.push_back(std::move(seq_init));

    // Add to extended samples with variant info
    for (const auto& mapping : state_inits_.back().sequence_sample_mappings) {
        RequiredSample req_sample;
        req_sample.name = mapping.sample_name;
        req_sample.bank = mapping.bank;
        req_sample.variant = mapping.variant;

        std::string key = req_sample.key();
        if (required_samples_extended_keys_.find(key) == required_samples_extended_keys_.end()) {
            required_samples_extended_keys_.insert(key);
            required_samples_extended_.push_back(std::move(req_sample));
        }
    }

    // Store pattern field buffers for %.field access
    std::unordered_map<std::string, std::uint16_t> pattern_fields;
    pattern_fields["freq"] = value_buf;
    pattern_fields["vel"] = velocity_buf;
    pattern_fields["trig"] = trigger_buf;
    record_fields_[node] = std::move(pattern_fields);

    pop_path();
    node_buffers_[node] = value_buf;
    return value_buf;
}

std::uint16_t CodeGenerator::handle_transport_call(NodeIndex node, const Node& n) {
    // transport(pattern, trig, step?, reset?) - trigger-driven pattern clock
    // Decouples pattern from global BPM by using trigger edges to advance

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "transport() requires a pattern as first argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& pat_node = ast_->arena[pattern_arg];
    if (pat_node.type != NodeType::MiniLiteral && !is_pattern_expr(*ast_, pattern_arg)) {
        error("E133", "transport() first argument must be a pattern", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Second arg: trigger signal (required)
    NodeIndex trig_arg = get_pattern_arg(*ast_, n, 1);
    if (trig_arg == NULL_NODE) {
        error("E131", "transport() requires a trigger signal as second argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Third arg: step size (optional, default 1.0)
    NodeIndex step_arg = get_pattern_arg(*ast_, n, 2);

    // Fourth arg: reset trigger (optional)
    NodeIndex reset_arg = get_pattern_arg(*ast_, n, 3);

    // Compile the pattern
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    std::uint32_t num_elements = 1;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, num_elements)) {
        error("E130", "transport() failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Set up state IDs
    std::uint32_t transport_count = call_counters_["transport"]++;
    push_path("transport#" + std::to_string(transport_count));
    std::uint32_t transport_state_id = compute_state_id();

    push_path("pat");
    std::uint32_t seq_state_id = compute_state_id();
    pop_path();

    float cycle_length = static_cast<float>(std::max(1u, num_elements));
    bool is_sample_pattern = compiler.is_sample_pattern();

    // Visit trigger argument
    std::uint16_t trig_buf = visit(trig_arg);
    if (trig_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Failed to compile trigger argument", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Visit optional step argument
    std::uint16_t step_buf = BufferAllocator::BUFFER_UNUSED;
    if (step_arg != NULL_NODE) {
        step_buf = visit(step_arg);
    }

    // Visit optional reset argument
    std::uint16_t reset_buf = BufferAllocator::BUFFER_UNUSED;
    if (reset_arg != NULL_NODE) {
        reset_buf = visit(reset_arg);
    }

    // Allocate beat_pos output buffer
    std::uint16_t beat_pos_buf = buffers_.allocate();
    if (beat_pos_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Pack cycle_length into two uint16_t halves
    std::uint32_t cl_bits = std::bit_cast<std::uint32_t>(cycle_length);
    std::uint16_t cl_lo = static_cast<std::uint16_t>(cl_bits & 0xFFFF);
    std::uint16_t cl_hi = static_cast<std::uint16_t>((cl_bits >> 16) & 0xFFFF);

    // Emit SEQPAT_TRANSPORT
    cedar::Instruction transport_inst{};
    transport_inst.opcode = cedar::Opcode::SEQPAT_TRANSPORT;
    transport_inst.out_buffer = beat_pos_buf;
    transport_inst.inputs[0] = trig_buf;
    transport_inst.inputs[1] = step_buf;
    transport_inst.inputs[2] = reset_buf;
    transport_inst.inputs[3] = cl_lo;
    transport_inst.inputs[4] = cl_hi;
    transport_inst.state_id = transport_state_id;
    emit(transport_inst);

    // Emit SEQPAT_QUERY with clock override
    cedar::Instruction query_inst{};
    query_inst.opcode = cedar::Opcode::SEQPAT_QUERY;
    query_inst.out_buffer = 0xFFFF;
    query_inst.inputs[0] = beat_pos_buf;  // Clock override
    query_inst.inputs[1] = 0xFFFF;
    query_inst.inputs[2] = 0xFFFF;
    query_inst.inputs[3] = 0xFFFF;
    query_inst.inputs[4] = 0xFFFF;
    query_inst.state_id = seq_state_id;
    emit(query_inst);

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // Allocate buffers for pattern outputs
    std::uint16_t value_buf = buffers_.allocate();
    std::uint16_t velocity_buf = buffers_.allocate();
    std::uint16_t trigger_buf = buffers_.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit per-voice SEQPAT_STEP/GATE/TYPE with clock override
    std::uint8_t max_voices = compiler.max_voices();
    if (!emit_per_voice_seqpat(node, seq_state_id, max_voices, value_buf, velocity_buf,
                                trigger_buf, is_sample_pattern, n.location, beat_pos_buf)) {
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Store sequence program initialization data
    auto sequence_events = compiler.sequence_events();
    StateInitData seq_init;
    seq_init.state_id = seq_state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    const Node& pattern = ast_->arena[pattern_node];
    seq_init.pattern_location = pattern.location;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits_.push_back(std::move(seq_init));

    pop_path();
    node_buffers_[node] = value_buf;
    return value_buf;
}

std::uint16_t CodeGenerator::handle_soundfont_call(NodeIndex node, const Node& n) {
    // soundfont(pattern, "file.sf2", preset) - SoundFont playback
    // The pattern provides gate, freq, velocity signals via polyphonic fields.
    // The file is a string literal (SF2 filename) resolved at compile time.
    // The preset is a number literal (index into the SF2's preset list).
    //
    // Usage: pat("c4 e4 g4") |> soundfont(%, "piano.sf2", 0) |> out(%, %)

    // Arg 0: pattern input (from pipe via %)
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "soundfont() requires a pattern as first argument "
              "(e.g., pat(\"c4 e4 g4\") |> soundfont(%, \"piano.sf2\", 0))", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Arg 1: filename (string literal)
    auto filename = get_string_arg(*ast_, n, 1);
    if (!filename.has_value()) {
        error("E131", "soundfont() requires a filename string as second argument "
              "(e.g., \"piano.sf2\")", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Arg 2: preset index (number literal)
    auto preset_opt = get_number_arg(*ast_, n, 2);
    if (!preset_opt.has_value()) {
        error("E132", "soundfont() requires a preset number as third argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    int preset_index = static_cast<int>(*preset_opt);

    // Visit pattern argument — this compiles the pattern and creates record fields
    std::uint16_t pattern_buf = visit(pattern_arg);
    if (pattern_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Failed to compile pattern argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Get pattern's record fields (gate, freq, vel)
    auto fields_it = record_fields_.find(pattern_arg);
    if (fields_it == record_fields_.end()) {
        error("E133", "soundfont() first argument must be a pattern that produces "
              "gate/freq/vel fields", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const auto& fields = fields_it->second;
    auto gate_it = fields.find("gate");
    auto freq_it = fields.find("freq");
    auto vel_it = fields.find("vel");

    if (gate_it == fields.end() || freq_it == fields.end() || vel_it == fields.end()) {
        error("E133", "soundfont() pattern is missing required fields (gate, freq, vel)",
              n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t gate_buf = gate_it->second;
    std::uint16_t freq_buf = freq_it->second;
    std::uint16_t vel_buf = vel_it->second;

    // Find or assign SF2 slot index (deduplicate by filename)
    std::uint8_t sf_slot = 0;
    bool found_slot = false;
    for (std::size_t i = 0; i < required_soundfonts_.size(); i++) {
        if (required_soundfonts_[i].filename == *filename) {
            sf_slot = static_cast<std::uint8_t>(i);
            found_slot = true;
            break;
        }
    }
    if (!found_slot) {
        sf_slot = static_cast<std::uint8_t>(required_soundfonts_.size());
        required_soundfonts_.push_back(RequiredSoundFont{*filename, preset_index});
    }

    // Create unique state ID
    std::uint32_t sf_count = call_counters_["soundfont"]++;
    push_path("soundfont#" + std::to_string(sf_count));
    std::uint32_t state_id = compute_state_id();

    // Emit constant for preset index
    std::uint16_t preset_buf = codegen::emit_push_const(buffers_, instructions_,
                                                         static_cast<float>(preset_index));
    if (preset_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }
    source_locations_.push_back(current_source_loc_);

    // Allocate output buffer
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Emit SOUNDFONT_VOICE instruction
    cedar::Instruction sf_inst{};
    sf_inst.opcode = cedar::Opcode::SOUNDFONT_VOICE;
    sf_inst.out_buffer = out_buf;
    sf_inst.inputs[0] = gate_buf;     // Gate signal (sustained)
    sf_inst.inputs[1] = freq_buf;     // Frequency in Hz
    sf_inst.inputs[2] = vel_buf;      // Velocity (0-1)
    sf_inst.inputs[3] = preset_buf;   // Preset index (constant)
    sf_inst.inputs[4] = 0xFFFF;
    sf_inst.state_id = state_id;
    sf_inst.rate = sf_slot;           // SF2 file index (resolved to sf_id at runtime)
    emit(sf_inst);

    pop_path();
    node_buffers_[node] = out_buf;
    return out_buf;
}

} // namespace akkado

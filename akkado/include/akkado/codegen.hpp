#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"
#include "symbol_table.hpp"
#include "sample_registry.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace akkado {

// ============================================================================
// Compiler Options (set by directives like $polyphony)
// ============================================================================

/// Compiler options that can be configured via directives
struct CompilerOptions {
    std::uint8_t default_polyphony = 4;  // Default voice count for polyphonic patterns
};

// ============================================================================
// Parameter Exposure System
// ============================================================================

/// Type of exposed parameter for UI generation
enum class ParamType : std::uint8_t {
    Continuous = 0,  // Float value in range [min, max] - rendered as slider
    Button = 1,      // Momentary: 1 while pressed, 0 otherwise
    Toggle = 2,      // Boolean: 0 or 1, click to flip
    Select = 3       // Discrete: integer index into options array
};

/// Declaration of an exposed parameter extracted at compile time
/// Used for auto-generating UI controls and external binding (Godot, MIDI, etc.)
struct ParamDecl {
    std::string name;              // Display name and EnvMap key
    std::uint32_t name_hash = 0;   // FNV-1a hash for ENV_GET lookup
    ParamType type = ParamType::Continuous;
    float default_value = 0.0f;    // Initial value
    float min_value = 0.0f;        // Minimum (Continuous only)
    float max_value = 1.0f;        // Maximum (Continuous only)
    std::vector<std::string> options;  // Option names (Select only)

    // Source location for UI linking (click-to-source)
    std::uint32_t source_offset = 0;   // Byte offset in source
    std::uint32_t source_length = 0;   // Length in source
};

// ============================================================================
// Visualization Exposure System
// ============================================================================

/// Type of visualization widget for UI generation
enum class VisualizationType : std::uint8_t {
    PianoRoll = 0,   // Musical pattern display with notes/events
    Oscilloscope = 1, // Time-domain waveform (short window, real-time)
    Waveform = 2,     // Time-domain waveform (longer window)
    Spectrum = 3      // Frequency-domain FFT display
};

/// Declaration of a visualization widget extracted at compile time
/// Used for generating UI visualization blocks below source lines
struct VisualizationDecl {
    std::string name;              // Display name
    VisualizationType type = VisualizationType::PianoRoll;
    std::uint32_t state_id = 0;    // For probe-based visualizations (oscilloscope, etc.)
    std::string options_json;      // Optional JSON configuration string

    // Source location for placing widget below the correct line
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;

    // For PianoRoll: link to the pattern's state_init index
    // (allows widget to access pattern events without duplication)
    std::int32_t pattern_state_init_index = -1;
};

// ============================================================================
// State Initialization Data
// ============================================================================

/// Sample name mapping for SequenceProgram deferred resolution
/// Tracks which events in which sequences need sample ID resolution
struct SequenceSampleMapping {
    std::uint16_t seq_idx;    // Index into sequences vector
    std::uint16_t event_idx;  // Index into sequence's events array
    std::string sample_name;  // Sample name to resolve
    std::string bank;         // Bank name (empty = default)
    std::uint8_t variant = 0; // Variant index (0 = first variant)
};

/// State initialization data for TIMELINE and SEQPAT_QUERY opcodes
struct StateInitData {
    std::uint32_t state_id;  // Must match Instruction::state_id (32-bit FNV-1a hash)
    enum class Type : std::uint8_t {
        Timeline = 1,     // Initialize TimelineState with breakpoints
        SequenceProgram   // Initialize SequenceState with compiled sequences
    } type;

    // Cycle length in beats (used by SequenceProgram)
    float cycle_length = 4.0f;

    // For Timeline: [time, value, curve, ...] triplets (existing usage)

    // For SequenceProgram: compiled sequences for lazy query
    // Note: The Sequence structs contain pointers to event vectors stored here
    std::vector<cedar::Sequence> sequences;  // Compiled sequence data (shallow copy)
    std::vector<std::vector<cedar::Event>> sequence_events;  // Actual event storage per sequence
    bool is_sample_pattern = false;  // Sample pattern vs pitch pattern

    // Size hints for arena allocation
    std::uint32_t total_events = 0;  // Total event count across all sequences

    // For SequenceProgram: sample name mappings for deferred resolution
    std::vector<SequenceSampleMapping> sequence_sample_mappings;

    // Pattern string location in document (for UI highlighting)
    SourceLocation pattern_location;  // Document offset of pattern string

    // AST JSON (serialized during compilation for debug UI)
    std::string ast_json;
};

/// Required sample with bank context
/// Used for runtime sample resolution with bank support
struct RequiredSample {
    std::string bank;      // Bank name (empty = default)
    std::string name;      // Sample name (e.g., "bd", "snare")
    int variant = 0;       // Variant index (0 = first variant)

    /// Get a unique key for deduplication
    [[nodiscard]] std::string key() const {
        if (bank.empty()) {
            return variant > 0 ? name + ":" + std::to_string(variant) : name;
        }
        return bank + "/" + name + ":" + std::to_string(variant);
    }

    /// Get the qualified sample name for Cedar (e.g., "TR808_bd_0")
    [[nodiscard]] std::string qualified_name() const {
        if (bank.empty() || bank == "default") {
            return variant > 0 ? name + ":" + std::to_string(variant) : name;
        }
        return bank + "_" + name + "_" + std::to_string(variant);
    }
};

/// Result of code generation
struct CodeGenResult {
    std::vector<cedar::Instruction> instructions;
    std::vector<SourceLocation> source_locations;  // Parallel to instructions, tracks origin
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data
    std::vector<std::string> required_samples;  // Unique sample names used - legacy
    std::vector<RequiredSample> required_samples_extended;  // Sample refs with bank/variant info
    std::vector<ParamDecl> param_decls;  // Declared parameters for UI generation
    std::vector<VisualizationDecl> viz_decls;  // Declared visualizations for UI generation
    bool success = false;
};

/// Buffer allocator for code generation
/// Simple linear allocation with no reuse (MVP)
class BufferAllocator {
public:
    static constexpr std::uint16_t MAX_BUFFERS = 256;
    static constexpr std::uint16_t BUFFER_UNUSED = 0xFFFF;
    // Buffer 255 is reserved for BUFFER_ZERO (always contains 0.0)
    static constexpr std::uint16_t MAX_ALLOCATABLE = 255;

    BufferAllocator() = default;

    /// Allocate a new buffer
    /// Returns BUFFER_UNUSED if pool exhausted
    [[nodiscard]] std::uint16_t allocate();

    /// Get current allocation count
    [[nodiscard]] std::uint16_t count() const { return next_; }

    /// Check if any buffers available
    [[nodiscard]] bool has_available() const { return next_ < MAX_ALLOCATABLE; }

private:
    std::uint16_t next_ = 0;
};

/// Code generator: converts analyzed AST to Cedar bytecode
class CodeGenerator {
public:
    /// Generate bytecode from analyzed AST
    /// @param ast The transformed AST (after pipe rewriting)
    /// @param symbols Symbol table from semantic analysis
    /// @param filename Filename for error reporting
    /// @param sample_registry Optional sample registry for resolving sample names to IDs
    CodeGenResult generate(const Ast& ast, SymbolTable& symbols,
                          std::string_view filename = "<input>",
                          SampleRegistry* sample_registry = nullptr);

private:
    /// Visit AST node and emit instructions
    /// Returns the buffer index containing this node's result
    std::uint16_t visit(NodeIndex node);

    /// Emit a single instruction
    void emit(const cedar::Instruction& inst);

    /// Generate semantic ID from path
    [[nodiscard]] std::uint32_t compute_state_id() const;

    /// Push/pop semantic path for nested structures
    void push_path(std::string_view segment);
    void pop_path();

    /// Error helpers
    void error(const std::string& code, const std::string& message, SourceLocation loc);

    /// Warning helper - emits a warning diagnostic but does not stop compilation
    void warn(const std::string& code, const std::string& message, SourceLocation loc);

    /// Check if a match expression can be resolved at compile time
    /// Returns true if scrutinee (if present) is const and all patterns/guards are const
    [[nodiscard]] bool is_compile_time_match(NodeIndex node, const Node& n) const;

    /// Handle compile-time match - evaluate patterns and guards, emit only winning branch
    std::uint16_t handle_compile_time_match(NodeIndex node, const Node& n);

    /// Handle runtime match - emit all branches and build nested select chain
    std::uint16_t handle_runtime_match(NodeIndex node, const Node& n);

    /// FM Detection: Automatically upgrade oscillators to 4x when FM is detected
    /// @param freq_buffer The buffer index containing the frequency input
    /// @return true if the frequency input traces back to an audio-rate source
    /// Note: is_audio_rate_producer, is_upgradeable_oscillator, upgrade_for_fm
    /// are inline helpers in akkado/codegen/fm_detection.hpp
    [[nodiscard]] bool is_fm_modulated(std::uint16_t freq_buffer) const;

    /// Handle len() function calls - compile-time array length
    /// Returns the number of elements in an array literal.
    /// @param node The Call node
    /// @param n The Node reference
    /// @return Output buffer index with constant length value
    std::uint16_t handle_len_call(NodeIndex node, const Node& n);

    /// Handle user-defined function calls - inline expansion
    /// @param node The Call node
    /// @param n The Node reference
    /// @param func The user function info from symbol table
    /// @return Output buffer index
    std::uint16_t handle_user_function_call(NodeIndex node, const Node& n,
                                            const UserFunctionInfo& func);

    /// Handle FunctionValue calls - inline expansion of lambda assigned to variable
    /// @param node The Call node
    /// @param n The Node reference
    /// @param func The function reference from symbol table
    /// @return Output buffer index
    std::uint16_t handle_function_value_call(NodeIndex node, const Node& n,
                                              const FunctionRef& func);

    /// Handle Closure nodes - allocate buffers for parameters and generate body
    /// @param node The Closure node
    /// @param n The Node reference
    /// @return Output buffer index
    std::uint16_t handle_closure(NodeIndex node, const Node& n);

    /// Handle MatchExpr nodes - compile-time match resolution
    /// @param node The MatchExpr node
    /// @param n The Node reference
    /// @return Output buffer index
    std::uint16_t handle_match_expr(NodeIndex node, const Node& n);

    /// Handle pattern variable reference
    /// Emits pattern code for the stored pattern.
    /// @param name The pattern variable name (for path tracking)
    /// @param pattern_node The MiniLiteral node index
    /// @param loc Source location for error reporting
    /// @return Output buffer index (pitch or sample_id)
    std::uint16_t handle_pattern_reference(const std::string& name, NodeIndex pattern_node,
                                           SourceLocation loc);

    /// Handle chord() function calls - Strudel-compatible chord expansion
    /// chord("Am") -> array of MIDI notes
    /// chord("Am C7 F") -> pattern of chord arrays
    /// @param node The Call node
    /// @param n The Node reference
    /// @return Output buffer index
    std::uint16_t handle_chord_call(NodeIndex node, const Node& n);

    /// Handle MiniLiteral (pattern) nodes - pat("c4 e4 g4"), etc.
    std::uint16_t handle_mini_literal(NodeIndex node, const Node& n);

    /// Emit per-voice SEQPAT_STEP/GATE/TYPE instructions and register polyphonic fields
    /// Shared helper used by handle_mini_literal and handle_pattern_reference
    /// @param node The node to register polyphonic fields / multi-buffers on
    /// @param state_id The state_id for the SEQPAT instructions
    /// @param max_voices Number of voices to emit
    /// @param value_buf Pre-allocated buffer for voice 0 value
    /// @param velocity_buf Pre-allocated buffer for voice 0 velocity
    /// @param trigger_buf Pre-allocated buffer for voice 0 trigger
    /// @param is_sample_pattern Whether this is a sample pattern (skips polyphonic registration)
    /// @param loc Source location for error reporting
    /// @return true on success, false on buffer exhaustion
    bool emit_per_voice_seqpat(NodeIndex node, std::uint32_t state_id,
                               std::uint8_t max_voices,
                               std::uint16_t value_buf, std::uint16_t velocity_buf,
                               std::uint16_t trigger_buf,
                               bool is_sample_pattern, SourceLocation loc);

    // ============================================================================
    // Pattern transformation handlers
    // ============================================================================

    /// Handle slow(pattern, factor) - stretch pattern by factor
    std::uint16_t handle_slow_call(NodeIndex node, const Node& n);

    /// Handle fast(pattern, factor) - compress pattern by factor
    std::uint16_t handle_fast_call(NodeIndex node, const Node& n);

    /// Handle rev(pattern) - reverse event order
    std::uint16_t handle_rev_call(NodeIndex node, const Node& n);

    /// Handle transpose(pattern, semitones) - shift pitches by semitones
    std::uint16_t handle_transpose_call(NodeIndex node, const Node& n);

    /// Handle velocity(pattern, vel) - set velocity on all events
    std::uint16_t handle_velocity_call(NodeIndex node, const Node& n);

    /// Handle bank(pattern, bank_name) - set sample bank for all events
    std::uint16_t handle_bank_call(NodeIndex node, const Node& n);

    /// Handle n(pattern, variant) - set sample variant for all events
    std::uint16_t handle_n_call(NodeIndex node, const Node& n);

    /// Handle tap_delay(in, time, fb, processor) - tap delay with inline feedback chain
    /// Emits DELAY_TAP, compiles processor closure inline, then emits DELAY_WRITE
    std::uint16_t handle_tap_delay_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Stereo handlers
    // ============================================================================

    /// Handle stereo(mono) or stereo(left, right) - create stereo signal
    std::uint16_t handle_stereo_call(NodeIndex node, const Node& n);

    /// Handle left(stereo) - extract left channel
    std::uint16_t handle_left_call(NodeIndex node, const Node& n);

    /// Handle right(stereo) - extract right channel
    std::uint16_t handle_right_call(NodeIndex node, const Node& n);

    /// Handle pan(mono, pos) - pan mono to stereo
    std::uint16_t handle_pan_call(NodeIndex node, const Node& n);

    /// Handle width(stereo, amount) - adjust stereo width (convenience wrapper)
    std::uint16_t handle_width_call(NodeIndex node, const Node& n);

    /// Handle ms_encode(stereo) - convert to mid/side (convenience wrapper)
    std::uint16_t handle_ms_encode_call(NodeIndex node, const Node& n);

    /// Handle ms_decode(ms) - convert mid/side to stereo (convenience wrapper)
    std::uint16_t handle_ms_decode_call(NodeIndex node, const Node& n);

    /// Handle pingpong(stereo, time, fb) - true stereo ping-pong delay
    std::uint16_t handle_pingpong_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Parameter exposure handlers
    // ============================================================================

    /// Handle param(name, default, min?, max?) - continuous slider parameter
    std::uint16_t handle_param_call(NodeIndex node, const Node& n);

    /// Handle button(name) - momentary button parameter
    std::uint16_t handle_button_call(NodeIndex node, const Node& n);

    /// Handle toggle(name, default?) - boolean toggle parameter
    std::uint16_t handle_toggle_call(NodeIndex node, const Node& n);

    /// Handle select(name, opt1, opt2, ...) - selection dropdown parameter
    std::uint16_t handle_select_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Visualization exposure handlers
    // ============================================================================

    /// Handle pianoroll(signal, name?) - attach piano roll visualization
    std::uint16_t handle_pianoroll_call(NodeIndex node, const Node& n);

    /// Handle oscilloscope(signal, name?) - attach oscilloscope visualization
    std::uint16_t handle_oscilloscope_call(NodeIndex node, const Node& n);

    /// Handle waveform(signal, name?) - attach waveform visualization
    std::uint16_t handle_waveform_call(NodeIndex node, const Node& n);

    /// Handle spectrum(signal, name?) - attach spectrum visualization
    std::uint16_t handle_spectrum_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Directive handlers
    // ============================================================================

    /// Handle compiler directives ($polyphony, etc.)
    /// Processes the directive and updates compiler options.
    /// @return BUFFER_UNUSED (directives don't produce values)
    std::uint16_t handle_directive(NodeIndex node, const Node& n);

    // Context
    CompilerOptions options_;  // Compiler options set by directives
    const Ast* ast_ = nullptr;
    SymbolTable* symbols_ = nullptr;
    SampleRegistry* sample_registry_ = nullptr;
    BufferAllocator buffers_;
    std::vector<cedar::Instruction> instructions_;
    std::vector<SourceLocation> source_locations_;  // Parallel to instructions_
    std::vector<Diagnostic> diagnostics_;
    std::vector<StateInitData> state_inits_;  // State initialization data
    std::vector<ParamDecl> param_decls_;      // Declared parameters
    std::vector<VisualizationDecl> viz_decls_;  // Declared visualizations
    std::string filename_;
    SourceLocation current_source_loc_;  // Current source location for emitted instructions

    // Semantic path tracking for state_id generation
    std::vector<std::string> path_stack_;
    std::uint32_t anonymous_counter_ = 0;

    // Track call counts per stateful function for unique state_ids
    std::unordered_map<std::string, std::uint32_t> call_counters_;

    // Track unique sample names used (for runtime loading) - legacy
    std::set<std::string> required_samples_;
    // Track samples with bank/variant info for extended sample resolution
    std::set<std::string> required_samples_extended_keys_;  // For deduplication
    std::vector<RequiredSample> required_samples_extended_;

    // Map from AST node index to output buffer index
    std::unordered_map<NodeIndex, std::uint16_t> node_buffers_;

    // Map from parameter name hash to literal AST node (for inline match resolution)
    // Only populated during user function calls when the argument is a literal
    std::unordered_map<std::uint32_t, NodeIndex> param_literals_;

    // Map from parameter name hash to FunctionRef (for closure parameters in user functions)
    // When a closure or function reference is passed as an argument to a user function,
    // this map stores the FunctionRef so it can be bound as FunctionValue in the function scope.
    std::unordered_map<std::uint32_t, FunctionRef> param_function_refs_;

    // Map from parameter name hash to argument node index (for multi-buffer propagation)
    // When a multi-buffer argument is passed to a function, this maps the parameter name
    // to the original argument node so that multi-buffer info can be looked up later.
    std::unordered_map<std::uint32_t, NodeIndex> param_multi_buffer_sources_;

    // ============================================================================
    // Multi-buffer support for polyphonic arrays (map/sum)
    // ============================================================================
    // Track nodes that produce multiple buffers (arrays/chords for polyphony)
    std::unordered_map<NodeIndex, std::vector<std::uint16_t>> multi_buffers_;

    // ============================================================================
    // Stereo signal tracking
    // ============================================================================
    // Track nodes that produce stereo signals (2-buffer pairs: left, right)
    struct StereoBuffers {
        std::uint16_t left;
        std::uint16_t right;
    };
    std::unordered_map<NodeIndex, StereoBuffers> stereo_outputs_;

    // Track stereo by buffer index (for variable propagation)
    // Key is the left buffer index, value is the right buffer index
    std::unordered_map<std::uint16_t, std::uint16_t> stereo_buffer_pairs_;

    /// Register a node as producing a stereo signal (2 buffers)
    void register_stereo(NodeIndex node, std::uint16_t left, std::uint16_t right);

    /// Check if a node produces a stereo signal
    [[nodiscard]] bool is_stereo(NodeIndex node) const;

    /// Check if a buffer index is the left channel of a stereo pair
    [[nodiscard]] bool is_stereo_buffer(std::uint16_t buffer) const;

    /// Get stereo buffer pair for a node (must check is_stereo first)
    [[nodiscard]] StereoBuffers get_stereo_buffers(NodeIndex node) const;

    /// Get stereo buffer pair by left buffer index
    [[nodiscard]] StereoBuffers get_stereo_buffers_by_buffer(std::uint16_t buffer) const;

    /// Register a node as producing multiple buffers
    /// @return First buffer index (for compatibility with single-buffer code)
    std::uint16_t register_multi_buffer(NodeIndex node, std::vector<std::uint16_t> buffers);

    /// Check if a node produces multiple buffers
    [[nodiscard]] bool is_multi_buffer(NodeIndex node) const;

    /// Get all buffers produced by a multi-buffer node
    [[nodiscard]] std::vector<std::uint16_t> get_multi_buffers(NodeIndex node) const;

    /// Apply a lambda expression to a single buffer value
    /// Creates a temporary scope, binds the parameter, generates body code
    /// @param lambda_node The Closure node containing parameter and body
    /// @param arg_buf The buffer to bind to the lambda parameter
    /// @return Output buffer from the lambda body
    std::uint16_t apply_lambda(NodeIndex lambda_node, std::uint16_t arg_buf);

    /// Resolve a function argument (can be inline lambda, lambda variable, or fn name)
    /// @param func_node The node that should resolve to a function
    /// @return FunctionRef if valid, nullopt otherwise
    std::optional<FunctionRef> resolve_function_arg(NodeIndex func_node);

    /// Apply a resolved function reference with captures
    /// @param ref The function reference containing closure/body info and captures
    /// @param arg_buf The buffer to bind to the function's first parameter
    /// @param loc Source location for error reporting
    /// @return Output buffer from the function body
    std::uint16_t apply_function_ref(const FunctionRef& ref, std::uint16_t arg_buf,
                                      SourceLocation loc);

    /// Handle map(array, fn) call - apply function to each element
    std::uint16_t handle_map_call(NodeIndex node, const Node& n);

    /// Handle sum(array) call - reduce array by addition
    std::uint16_t handle_sum_call(NodeIndex node, const Node& n);

    /// Handle fold(array, fn, init) call - reduce array with binary function
    std::uint16_t handle_fold_call(NodeIndex node, const Node& n);

    /// Handle zipWith(a, b, fn) call - combine arrays element-wise with function
    std::uint16_t handle_zipWith_call(NodeIndex node, const Node& n);

    /// Handle zip(a, b) call - interleave two arrays
    std::uint16_t handle_zip_call(NodeIndex node, const Node& n);

    /// Handle take(n, array) call - take first n elements
    std::uint16_t handle_take_call(NodeIndex node, const Node& n);

    /// Handle drop(n, array) call - drop first n elements
    std::uint16_t handle_drop_call(NodeIndex node, const Node& n);

    /// Handle reverse(array) call - reverse array order
    std::uint16_t handle_reverse_call(NodeIndex node, const Node& n);

    /// Handle range(start, end) call - generate integer array
    std::uint16_t handle_range_call(NodeIndex node, const Node& n);

    /// Handle repeat(value, n) call - repeat value n times
    std::uint16_t handle_repeat_call(NodeIndex node, const Node& n);

    /// Handle spread(n, source) call - force specific voice count
    std::uint16_t handle_spread_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Array reduction operations
    // ============================================================================

    /// Handle product(array) call - multiply all elements
    std::uint16_t handle_product_call(NodeIndex node, const Node& n);

    /// Handle mean(array) call - average of elements
    std::uint16_t handle_mean_call(NodeIndex node, const Node& n);

    /// Handle min/max calls - either binary or reduction depending on args
    std::uint16_t handle_minmax_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Array transformation operations
    // ============================================================================

    /// Handle rotate(array, n) call - rotate elements by n positions
    std::uint16_t handle_rotate_call(NodeIndex node, const Node& n);

    /// Handle shuffle(array) call - deterministic random permutation
    std::uint16_t handle_shuffle_call(NodeIndex node, const Node& n);

    /// Handle sort(array) call - sort elements in ascending order
    std::uint16_t handle_sort_call(NodeIndex node, const Node& n);

    /// Handle normalize(array) call - scale to 0-1 range
    std::uint16_t handle_normalize_call(NodeIndex node, const Node& n);

    /// Handle scale(array, lo, hi) call - map to new range
    std::uint16_t handle_scale_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Array generation operations
    // ============================================================================

    /// Handle linspace(start, end, n) call - N evenly spaced values
    std::uint16_t handle_linspace_call(NodeIndex node, const Node& n);

    /// Handle random(n) call - N random values (deterministic by path)
    std::uint16_t handle_random_call(NodeIndex node, const Node& n);

    /// Handle harmonics(fundamental, n) call - harmonic series
    std::uint16_t handle_harmonics_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Binary operation broadcasting
    // ============================================================================

    /// Handle binary operations (add, sub, mul, div, pow) with array broadcasting
    std::uint16_t handle_binary_op_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Record support
    // ============================================================================

    /// Handle record literal nodes - expand to multiple buffers
    /// @param node The RecordLit node
    /// @param n The Node reference
    /// @return First buffer index (for single-buffer compatibility)
    std::uint16_t handle_record_literal(NodeIndex node, const Node& n);

    /// Handle field access nodes - resolve to correct field buffer
    /// @param node The FieldAccess node
    /// @param n The Node reference
    /// @return Buffer index of the accessed field
    std::uint16_t handle_field_access(NodeIndex node, const Node& n);

    /// Handle pipe binding nodes - create buffer alias for named binding
    /// @param node The PipeBinding node
    /// @param n The Node reference
    /// @return Buffer index of the bound expression
    std::uint16_t handle_pipe_binding(NodeIndex node, const Node& n);

    // Map from record node to field name -> buffer index
    // Used to track field buffers for record literals
    std::unordered_map<NodeIndex, std::unordered_map<std::string, std::uint16_t>> record_fields_;

    // ============================================================================
    // Polyphonic pattern support
    // ============================================================================
    // Track per-voice buffers for polyphonic patterns (chords, arrays)
    // Each field (freq, vel, trig) has one buffer per voice
    struct PolyphonicFields {
        std::vector<std::uint16_t> freq_buffers;   // Per-voice frequency
        std::vector<std::uint16_t> vel_buffers;    // Per-voice velocity
        std::vector<std::uint16_t> trig_buffers;   // Per-voice trigger
        std::vector<std::uint16_t> gate_buffers;   // Per-voice gate (sustained signal)
        std::vector<std::uint16_t> type_buffers;   // Per-voice type_id (for sample routing)
        std::uint8_t num_voices = 0;
    };
    std::unordered_map<NodeIndex, PolyphonicFields> polyphonic_fields_;

    /// Resolve a polyphonic field name to its buffer vector
    /// @param poly The polyphonic fields for the pattern
    /// @param field_name The field name to resolve (freq, vel, trig, gate, type + aliases)
    /// @param out_buffers Output: the buffer vector for the resolved field
    /// @return true if field was found, false otherwise
    [[nodiscard]] bool resolve_polyphonic_field(const PolyphonicFields& poly,
                                                const std::string& field_name,
                                                std::vector<std::uint16_t>& out_buffers) const;

    // ============================================================================
    // Array length tracking (compile-time)
    // ============================================================================
    // Track array lengths per buffer (0 = scalar audio buffer)
    // Arrays reuse BufferPool buffers - elements stored at indices 0..length-1
    std::unordered_map<std::uint16_t, std::uint8_t> array_lengths_;

    /// Check if a buffer holds an array (vs scalar audio signal)
    [[nodiscard]] bool is_array_buffer(std::uint16_t buf) const {
        auto it = array_lengths_.find(buf);
        return it != array_lengths_.end() && it->second > 0;
    }

    /// Get the length of an array buffer (0 if not an array)
    [[nodiscard]] std::uint8_t get_array_length(std::uint16_t buf) const {
        auto it = array_lengths_.find(buf);
        return (it != array_lengths_.end()) ? it->second : 0;
    }

    /// Register a buffer as holding an array of given length
    void set_array_length(std::uint16_t buf, std::uint8_t length) {
        if (length > 0) {
            array_lengths_[buf] = length;
        }
    }

    /// Apply a binary function reference to two buffer arguments
    /// @param ref The function reference containing closure/body info
    /// @param arg_buf1 The first argument buffer
    /// @param arg_buf2 The second argument buffer
    /// @param loc Source location for error reporting
    /// @return Output buffer from the function body
    std::uint16_t apply_binary_function_ref(const FunctionRef& ref,
                                            std::uint16_t arg_buf1,
                                            std::uint16_t arg_buf2,
                                            SourceLocation loc);
};

} // namespace akkado

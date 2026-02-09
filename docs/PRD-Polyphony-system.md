# Polyphony System PRD

## Executive Summary

This document specifies a complete polyphony system for Enkido/Akkado that enables:
- Multi-voice synthesis from pattern chords and MIDI input
- Per-voice DSP processing with automatic state isolation
- Per-type voice routing (drum machines, multi-timbral)
- Voice consolidation into shared effects chains
- Configurable voice count with `$polyphony(n)`
- Hot-swap compatible state preservation

---

## 1. Core Concepts

### 1.1 Voice Source

A **voice source** produces events that can trigger multiple simultaneous voices.

**Types**:
- `pat("C4' Am7'")` - Pattern with chords (compile-time analyzed)
- `chord("Am7")` - Single chord (compile-time)
- `midi_in()` - External MIDI (runtime varying)
- `osc_bank(n)` - N parallel oscillator voices (future)

**Event Fields** (accessible via `%.field`):
| Field | Aliases | Description |
|-------|---------|-------------|
| `freq` | `f`, `pitch` | Frequency in Hz |
| `vel` | `v`, `velocity` | Velocity 0-1 |
| `trig` | `t`, `trigger` | Trigger pulse (1 on note-on) |
| `gate` | `g` | Gate signal (1 while held, 0 on release) |
| `note` | `n`, `midi` | MIDI note number |
| `type` | | Voice type string (for per-type routing) |
| `dur` | `duration` | Event duration in beats |

### 1.2 Voice Expansion

When a voice source flows into a stateful UGen, the UGen is **automatically expanded** to N instances (one per voice).

```akkado
// 3 voices (C, E, G) → 3 oscillators
pat("C4'") |> sine(%.freq) |> sum(%) |> out(%, %)
```

**Expansion Rules**:
1. Voice source creates multi-buffer of voice event fields
2. Stateful UGen receiving multi-buffer expands to N instances
3. Each instance gets unique state_id from semantic path
4. Result is multi-buffer of N outputs

### 1.3 Voice Consolidation

Multiple voices are combined back to a single signal for shared processing:

```akkado
pat("C4' Am7'")
|> sine(%.freq) * env(%.trig)  // per-voice: 4 oscillators, 4 envelopes
|> sum(%)                           // consolidate: 4 → 1
|> reverb(%)                        // shared: 1 reverb instance
|> out(%, %)
```

**Consolidation Functions**:
- `sum(arr)` - Sum all voices
- `mean(arr)` - Average (sum / count)
- `mix(arr, weights)` - Weighted mix (future)
- `pan_spread(arr)` - Stereo spread across voices (future)

### 1.4 Per-Type Voice Routing

For drum machines or multi-timbral setups, different voice types can route to different DSP chains:

```akkado
pat("kick snare hihat kick snare")
|> match(%.type) {
    "kick": sine(55) * env(%.trig, 0.01, 0.1)
    "snare": noise() * env(%.trig, 0.01, 0.2) |> hp(200, %)
    "hihat": noise() * env(%.trig, 0.001, 0.05) |> hp(8000, %)
    _: 0
}
|> sum(%)
|> out(%, %)
```

**Type Resolution**:
- Pattern atoms map to type strings (e.g., "kick", "snare")
- Types can also be numeric indices (0, 1, 2...)
- `match(%.type)` routes each voice to appropriate DSP

---

## 2. Syntax & API

### 2.1 Global Directives

```akkado
$polyphony(16)      // Default voice count for MIDI/unknown sources
$sample_rate(48000) // (existing)
$bpm(120)           // (existing)
```

### 2.2 Voice Sources

```akkado
// Pattern-based (compile-time analyzed)
pat("C4' Am7'")           // Chords in mini-notation
chord("Am7")              // Single chord
[440, 550, 660]           // Array literal

// External input (runtime)
midi_in()                 // MIDI input, expands to $polyphony voices
midi_in(channel: 1)       // Specific MIDI channel

// Explicit voice count
spread(8, source)         // Force 8 voices from source
```

### 2.3 Voice Field Access

```akkado
pat("C4' Am7'") as e |>
    sine(e.freq) *     // e.freq = multi-buffer of all voice frequencies
    e.vel *                 // e.vel = multi-buffer of all voice velocities
    env(e.trig)             // e.trig = multi-buffer of all voice triggers
|> sum(%)
|> out(%, %)
```

### 2.4 Per-Voice vs Shared Processing

```akkado
// Per-voice: UGen inside voice expansion
pat("C4'") |> sine(%.freq) |> filter(1000, %) |> sum(%)
//           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 3 oscs, 3 filters

// Shared: UGen after consolidation
pat("C4'") |> sine(%.freq) |> sum(%) |> filter(1000, %)
//                                          ^^^^^^^^^^^^^^^^ 1 filter

// Mixed:
pat("C4'")
|> sine(%.freq) * env(%.trig)   // per-voice
|> sum(%) * 0.3                      // consolidate + scale
|> reverb(%)                         // shared
|> out(%, %)
```

### 2.5 Nested Voice Expansion

Voice expansion composes naturally:

```akkado
fn supersaw(freq) =
    [freq * 0.99, freq, freq * 1.01] |> saw(%) |> sum(%) * 0.33

// 4 voices × 3 oscs = 12 oscillators
pat("Am7'") |> supersaw(%.freq) |> sum(%) |> out(%, %)
```

---

## 3. Implementation Architecture

### 3.1 Compile-Time Voice Analysis

For pattern-based sources, analyze at compile time:

```cpp
// In SequenceCompiler (akkado/src/codegen_patterns.cpp)
std::uint8_t max_voices() const {
    std::uint8_t max = 1;
    for (const auto& seq : sequence_events_) {
        for (const auto& e : seq) {
            max = std::max(max, e.num_values);
        }
    }
    return max;
}
```

### 3.2 Multi-Buffer Voice Fields

**Current limitation**: `%.freq` returns only first voice's frequency.

**Fix**: Return multi-buffer of all voice frequencies.

```cpp
// In CodeGenerator (akkado/include/akkado/codegen.hpp)
struct PolyphonicFields {
    std::vector<std::uint16_t> freq_buffers;
    std::vector<std::uint16_t> vel_buffers;
    std::vector<std::uint16_t> trig_buffers;
    std::vector<std::uint16_t> gate_buffers;
};

// Map pattern node → polyphonic field buffers
std::unordered_map<NodeIndex, PolyphonicFields> polyphonic_fields_;
```

**Pattern codegen** (`akkado/src/codegen_patterns.cpp`):
```cpp
// For each voice, emit SEQPAT_STEP and collect buffers
PolyphonicFields fields;
for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
    // ... emit SEQPAT_STEP for this voice ...
    fields.freq_buffers.push_back(voice_freq_buf);
    fields.vel_buffers.push_back(voice_vel_buf);
    fields.trig_buffers.push_back(voice_trig_buf);
    fields.gate_buffers.push_back(voice_gate_buf);
}
polyphonic_fields_[node] = std::move(fields);
```

**Field access codegen** (`akkado/src/codegen.cpp`):
```cpp
std::uint16_t handle_field_access(NodeIndex node, const Node& n) {
    // ... existing code ...

    // Check for polyphonic pattern field
    auto poly_it = polyphonic_fields_.find(expr_node);
    if (poly_it != polyphonic_fields_.end()) {
        const auto& fields = poly_it->second;
        std::vector<std::uint16_t>* buffers = nullptr;

        if (field_name == "freq" || field_name == "f" || field_name == "pitch") {
            buffers = &fields.freq_buffers;
        } else if (field_name == "vel" || field_name == "v" || field_name == "velocity") {
            buffers = &fields.vel_buffers;
        } else if (field_name == "trig" || field_name == "t" || field_name == "trigger") {
            buffers = &fields.trig_buffers;
        } else if (field_name == "gate" || field_name == "g") {
            buffers = &fields.gate_buffers;
        }

        if (buffers && buffers->size() > 1) {
            return register_multi_buffer(node, *buffers);
        }
    }

    // ... fall through to existing handling ...
}
```

### 3.3 UGen Auto-Expansion (Already Implemented)

The existing UGen auto-expansion in `codegen.cpp:660-765` handles multi-buffer inputs:
- Detects when argument is multi-buffer
- Creates N instances with unique state_ids
- Each instance processes one element
- Returns multi-buffer of N outputs

### 3.4 MIDI Voice Allocation

**New VM State**: Voice slot manager

```cpp
// In cedar/include/cedar/vm/midi_voices.hpp
struct VoiceSlot {
    std::int8_t note = -1;      // MIDI note, -1 = inactive
    float freq = 0.0f;          // Frequency in Hz
    float vel = 0.0f;           // Velocity 0-1
    float gate = 0.0f;          // Gate signal
    std::uint32_t age = 0;      // For voice stealing
    bool releasing = false;     // In release phase
};

class VoiceAllocator {
    std::array<VoiceSlot, 128> slots_;  // Max 128 voices
    std::size_t active_count_ = 0;
    std::size_t max_voices_ = 16;       // From $polyphony

public:
    void set_max_voices(std::size_t n);
    std::size_t allocate(std::int8_t note, float vel);  // Returns slot index
    void release(std::int8_t note);                      // Note off
    const VoiceSlot& get(std::size_t idx) const;
    void tick();  // Increment ages, clean up released
};
```

**New Opcodes**:
```cpp
enum class Opcode : std::uint8_t {
    // ... existing ...

    // MIDI Voice (180-189)
    MIDI_VOICE_FREQ = 180,   // out = voice[slot_idx].freq
    MIDI_VOICE_VEL = 181,    // out = voice[slot_idx].vel
    MIDI_VOICE_GATE = 182,   // out = voice[slot_idx].gate
    MIDI_VOICE_TRIG = 183,   // out = voice[slot_idx] triggered this block
    MIDI_NOTE_ON = 184,      // Process note-on queue
    MIDI_NOTE_OFF = 185,     // Process note-off queue
};
```

**Akkado codegen for midi_in()**:
```cpp
std::uint16_t handle_midi_in(NodeIndex node, const Node& n) {
    std::size_t voice_count = options_.default_polyphony;  // From $polyphony

    // Create buffers for each voice slot
    PolyphonicFields fields;
    for (std::size_t i = 0; i < voice_count; ++i) {
        push_path("voice" + std::to_string(i));

        // Emit MIDI_VOICE_* instructions for this slot
        std::uint16_t freq_buf = emit_midi_voice_freq(i);
        std::uint16_t vel_buf = emit_midi_voice_vel(i);
        std::uint16_t trig_buf = emit_midi_voice_trig(i);
        std::uint16_t gate_buf = emit_midi_voice_gate(i);

        fields.freq_buffers.push_back(freq_buf);
        fields.vel_buffers.push_back(vel_buf);
        fields.trig_buffers.push_back(trig_buf);
        fields.gate_buffers.push_back(gate_buf);

        pop_path();
    }

    polyphonic_fields_[node] = std::move(fields);
    return register_multi_buffer(node, fields.freq_buffers);
}
```

### 3.5 Per-Type Voice Routing

**Pattern type annotation**:
In mini-notation, bare words become type strings:
- `pat("kick snare hihat")` → events with type="kick", type="snare", type="hihat"
- `pat("c4 e4 g4")` → events with type="" (no type, melodic)

**Type field in Event struct**:
```cpp
// In cedar/include/cedar/opcodes/sequence.hpp
struct Event {
    // ... existing fields ...
    std::uint8_t type_id = 0;  // Index into type string table
};
```

**SEQPAT_TYPE opcode**:
```cpp
SEQPAT_TYPE = 154,  // out = current event type_id (0-255)
```

**Match codegen**:
```cpp
// match(%.type) generates conditional branches
// Each arm compiles to: if type == arm_type then arm_body else next_arm
```

### 3.6 spread() Function

Force specific voice count, padding with zeros if source has fewer:

```cpp
std::uint16_t handle_spread_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    std::size_t target_count = static_cast<std::size_t>(
        ast_->arena[args.nodes[0]].as_number());
    NodeIndex source = args.nodes[1];

    // Visit source to get its voice buffers
    visit(source);
    auto source_buffers = get_multi_buffers(source);

    // Pad or truncate to target count
    std::vector<std::uint16_t> result;
    for (std::size_t i = 0; i < target_count; ++i) {
        if (i < source_buffers.size()) {
            result.push_back(source_buffers[i]);
        } else {
            // Pad with zero (silent voice)
            result.push_back(emit_const(0.0f, node));
        }
    }

    return register_multi_buffer(node, std::move(result));
}
```

### 3.7 Gate vs Trigger Fields

**trig** (%.trig): Impulse signal, 1.0 on note-on frame, 0.0 otherwise
- Use for: Triggering envelopes, one-shots

**gate** (%.gate): Level signal, 1.0 while note held, 0.0 after release
- Use for: Sustaining envelopes, gated processing

```akkado
// ADSR envelope with proper release
pat("C4' Am7'") as e |>
    sine(e.freq) * adsr(e.gate, 0.01, 0.1, 0.7, 0.3)
|> sum(%) |> out(%, %)
```

### 3.8 Hot-Swap State Preservation

State preservation uses semantic path matching:
- Each voice instance has path like `main/pat#0/voice2/osc#0`
- On hot-swap, match paths to preserve oscillator phase, filter state, etc.
- If voice structure changes, new voices start fresh

---

## 4. Directive System

### 4.1 Lexer Changes

```cpp
// akkado/src/lexer.cpp
case '$':
    advance();
    return Token{TokenType::DIRECTIVE, scan_identifier(), ...};
```

### 4.2 Parser Changes

```cpp
// akkado/src/parser.cpp
NodeIndex parse_directive() {
    // $polyphony(16)
    Token directive = previous();  // DIRECTIVE token
    consume(TokenType::LPAREN, "Expected '(' after directive");
    NodeIndex arg = parse_expression();
    consume(TokenType::RPAREN, "Expected ')' after directive argument");

    NodeIndex node = make_node(NodeType::Directive, directive);
    ast_->arena[node].data = Node::DirectiveData{
        directive.lexeme.substr(1),  // Remove '$'
        arg
    };
    return node;
}
```

### 4.3 Codegen Changes

```cpp
// akkado/src/codegen.cpp
struct CompilerOptions {
    std::size_t default_polyphony = 16;
    // ... other options ...
};

void handle_directive(NodeIndex node, const Node& n) {
    const auto& data = n.as_directive();
    if (data.name == "polyphony") {
        options_.default_polyphony = static_cast<std::size_t>(
            ast_->arena[data.arg].as_number());
    }
}
```

---

## 5. Web Integration

### 5.1 WebMIDI Integration

```typescript
// web/src/lib/midi/midi-manager.ts
class MIDIManager {
    private inputs: Map<string, MIDIInput> = new Map();

    async init() {
        const access = await navigator.requestMIDIAccess();
        for (const input of access.inputs.values()) {
            input.onmidimessage = this.handleMessage.bind(this);
            this.inputs.set(input.id, input);
        }
    }

    private handleMessage(event: MIDIMessageEvent) {
        const [status, note, velocity] = event.data;
        const channel = status & 0x0F;
        const command = status >> 4;

        if (command === 9 && velocity > 0) {
            // Note on
            audioStore.midiNoteOn(channel, note, velocity / 127);
        } else if (command === 8 || (command === 9 && velocity === 0)) {
            // Note off
            audioStore.midiNoteOff(channel, note);
        }
    }
}
```

### 5.2 WASM Bindings

```cpp
// web/wasm/enkido_wasm.cpp
EMSCRIPTEN_BINDINGS(enkido) {
    // ... existing ...

    function("cedar_midi_note_on", &cedar_midi_note_on);
    function("cedar_midi_note_off", &cedar_midi_note_off);
    function("cedar_set_polyphony", &cedar_set_polyphony);
}

void cedar_midi_note_on(int channel, int note, float velocity) {
    g_vm.voice_allocator().note_on(note, velocity);
}

void cedar_midi_note_off(int channel, int note) {
    g_vm.voice_allocator().note_off(note);
}

void cedar_set_polyphony(int max_voices) {
    g_vm.voice_allocator().set_max_voices(max_voices);
}
```

---

## 6. Files to Modify/Create

### 6.1 New Files

| File | Description |
|------|-------------|
| `cedar/include/cedar/vm/voice_allocator.hpp` | Voice slot manager for MIDI |
| `cedar/include/cedar/opcodes/midi.hpp` | MIDI_VOICE_* opcode implementations |
| `web/src/lib/midi/midi-manager.ts` | WebMIDI integration |

### 6.2 Modified Files

| File | Changes |
|------|---------|
| `akkado/include/akkado/codegen.hpp` | Add PolyphonicFields, CompilerOptions |
| `akkado/src/codegen.cpp` | Polyphonic field access, directive handling |
| `akkado/src/codegen_patterns.cpp` | Emit per-voice field buffers |
| `akkado/src/lexer.cpp` | DIRECTIVE token |
| `akkado/src/parser.cpp` | parse_directive() |
| `akkado/include/akkado/builtins.hpp` | spread, midi_in entries |
| `cedar/include/cedar/vm/instruction.hpp` | MIDI_VOICE_* opcodes |
| `cedar/include/cedar/vm/vm.hpp` | VoiceAllocator member |
| `cedar/include/cedar/opcodes/sequence.hpp` | type_id field in Event |
| `web/wasm/enkido_wasm.cpp` | MIDI bindings |

---

## 7. Implementation Phases

### Phase 1: Polyphonic Field Access (Foundation)
**Goal**: `pat("C4'") |> sine(%.freq) |> sum(%)` works

**Changes**:
1. Add PolyphonicFields struct to codegen.hpp
2. Modify pattern codegen to emit per-voice field buffers
3. Modify field access to return multi-buffer from polyphonic pattern
4. Add tests for polyphonic expansion

**Estimate**: ~200 lines C++

### Phase 2: Directive System
**Goal**: `$polyphony(16)` sets default

**Changes**:
1. Add DIRECTIVE token to lexer
2. Add parse_directive() to parser
3. Add CompilerOptions and directive handling to codegen
4. Add tests

**Estimate**: ~100 lines C++

### Phase 3: spread() Function
**Goal**: `spread(8, source)` forces voice count

**Changes**:
1. Add spread builtin entry
2. Implement handle_spread_call
3. Add tests

**Estimate**: ~50 lines C++

### Phase 4: Per-Type Routing
**Goal**: `match(%.type)` routes by voice type

**Changes**:
1. Add type_id field to Event struct
2. Add SEQPAT_TYPE opcode
3. Pattern parser extracts type from atom names
4. match() codegen handles type routing
5. Add tests

**Estimate**: ~200 lines C++

### Phase 5: Gate Field & Envelope Integration
**Goal**: `env(%.gate)` for proper ADSR

**Changes**:
1. Add gate_buffers to PolyphonicFields
2. Add SEQPAT_GATE opcode (tracks held state)
3. Modify pattern codegen to emit gate
4. Add tests

**Estimate**: ~100 lines C++

### Phase 6: MIDI Input
**Goal**: `midi_in() |> ...` works with external MIDI

**Changes**:
1. Create VoiceAllocator class
2. Add MIDI_VOICE_* opcodes
3. Implement handle_midi_in codegen
4. Add WebMIDI integration
5. Add WASM bindings
6. Add tests

**Estimate**: ~400 lines C++, ~150 lines TypeScript

---

## 8. Testing Strategy

### 8.1 Unit Tests (akkado/tests/)

```cpp
TEST_CASE("Polyphonic field access", "[polyphony]") {
    SECTION("%.freq on triad returns 3-element multi-buffer") {
        auto result = compile(R"(
            pat("C4'") |> sine(%.freq) |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(count_instructions(result, OSC_SIN) == 3);
    }

    SECTION("%.trig on chord triggers all voices") {
        auto result = compile(R"(
            pat("Am7'") |> sine(%.freq) * env(%.trig) |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(count_instructions(result, OSC_SIN) == 4);
        CHECK(count_instructions(result, ENV_AR) == 4);
    }

    SECTION("Nested expansion multiplies voice count") {
        auto result = compile(R"(
            fn supersaw(f) = [f*0.99, f, f*1.01] |> saw(%) |> sum(%)
            pat("C4'") |> supersaw(%.freq) |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(count_instructions(result, OSC_SAW) == 9);  // 3 voices × 3 oscs
    }
}

TEST_CASE("Directive system", "[directives]") {
    SECTION("$polyphony sets default voice count") {
        auto result = compile(R"(
            $polyphony(8)
        )");
        REQUIRE(result.success);
        CHECK(result.options.default_polyphony == 8);
    }
}

TEST_CASE("spread() function", "[polyphony]") {
    SECTION("spread pads to target count") {
        auto result = compile(R"(
            spread(6, pat("C4'")) |> sine(%.freq) |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(count_instructions(result, OSC_SIN) == 6);  // 3 real + 3 padded
    }
}

TEST_CASE("Per-type routing", "[polyphony][match]") {
    SECTION("match routes by type string") {
        auto result = compile(R"(
            pat("kick snare") |> match(%.type) {
                "kick": sine(55)
                "snare": noise()
            } |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);
        CHECK(count_instructions(result, OSC_SIN) == 1);
        CHECK(count_instructions(result, NOISE) == 1);
    }
}
```

### 8.2 Integration Tests (web/tests/)

```typescript
describe('Polyphony', () => {
    it('produces audio from chord pattern', async () => {
        const source = `pat("C4'") |> sine(%.freq) |> sum(%) * 0.3 |> out(%, %)`;
        compile_and_run(source);
        expect(getMaxOutput()).toBeGreaterThan(0);
    });

    it('handles nested expansion', async () => {
        const source = `
            fn super(f) = [f*0.99, f*1.01] |> saw(%) |> sum(%)
            pat("C4'") |> super(%.freq) |> sum(%) * 0.1 |> out(%, %)
        `;
        compile_and_run(source);
        expect(getMaxOutput()).toBeGreaterThan(0);
    });
});

describe('MIDI Input', () => {
    it('responds to MIDI note on', async () => {
        const source = `midi_in() |> sine(%.freq) * env(%.gate) |> sum(%) |> out(%, %)`;
        compile_and_run(source);

        enkido.cedar_midi_note_on(0, 60, 0.8);  // C4
        process_blocks(10);
        expect(getMaxOutput()).toBeGreaterThan(0);

        enkido.cedar_midi_note_off(0, 60);
        process_blocks(100);  // Let envelope release
        expect(getMaxOutput()).toBeLessThan(0.01);  // Faded to silence
    });
});
```

### 8.3 Manual Testing

```akkado
// Basic polyphonic chord
pat("C4' Am7' G7'") |> sine(%.freq) * env(%.trig) |> sum(%) * 0.2 |> out(%, %)

// Supersaw per voice (12 oscillators for Am7')
fn supersaw(f) = [f*0.99, f, f*1.01] |> saw(%) |> sum(%) * 0.33
pat("Am7'") |> supersaw(%.freq) * env(%.trig) |> sum(%) * 0.2 |> out(%, %)

// Drum machine
pat("kick . snare . kick kick snare .")
|> match(%.type) {
    "kick": sine(55) * env(%.trig, 0.01, 0.1)
    "snare": noise() * env(%.trig, 0.01, 0.2) |> hp(200, %)
}
|> sum(%) * 0.5
|> out(%, %)

// MIDI input
$polyphony(8)
midi_in() |> sine(%.freq) * env(%.gate, 0.01, 0.1, 0.7, 0.3) |> sum(%) * 0.3 |> out(%, %)
```

---

## 9. Summary

This PRD specifies a complete polyphony system with:

| Feature | Syntax | Status |
|---------|--------|--------|
| Polyphonic field access | `%.freq` returns multi-buffer | Phase 1 |
| Directive system | `$polyphony(16)` | Phase 2 |
| Voice count override | `spread(8, source)` | Phase 3 |
| Per-type routing | `match(%.type) {...}` | Phase 4 |
| Gate field | `%.gate` for sustain/release | Phase 5 |
| MIDI input | `midi_in()` | Phase 6 |

**Total estimated scope**: ~1000 lines C++, ~150 lines TypeScript

**Key design principles**:
1. Compile-time expansion where possible (patterns, chords)
2. Runtime allocation only for external input (MIDI)
3. Semantic path-based state preservation for hot-swap
4. Composable voice expansion (nested functions work naturally)
5. Explicit consolidation points (`sum()`, `mean()`)

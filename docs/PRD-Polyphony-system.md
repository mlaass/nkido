**Status: REFACTOR** — Old compile-time expansion system (Phases 1-5) complete. This PRD replaces it with a unified runtime POLY opcode. Phase 6 (MIDI) absorbed into the new design.

# Polyphony System PRD — Runtime POLY Opcode

## Executive Summary

This document specifies a unified runtime polyphony system for Enkido built around a single **POLY opcode** that manages voices for any instrument — oscillators, SoundFonts, samplers, FM synths, etc. It replaces the current three incompatible mechanisms with one explicit, composable approach.

### Why Refactor?

The current system has three separate, incompatible polyphony mechanisms:

| Mechanism | How it works | Problem |
|-----------|-------------|---------|
| **Compile-time multi-buffer expansion** | Compiler duplicates UGens N times when they receive polyphonic input | Implicit — polyphony determined by pattern content, no voice management, no mono/legato |
| **SoundFont internal voice allocator** | 32 voices baked into SOUNDFONT_VOICE opcode | Isolated — can't share voice management with other instruments |
| **Sampler internal voice allocator** | 32 voices baked into SAMPLE_PLAY opcode | Same isolation problem |

**Goal**: Users explicitly choose `poly(n, instrument)` or `mono(instrument)`. One voice allocator handles everything — oscillators, SoundFonts, samplers, FM synths, future instruments.

**Key design decisions:**
- SoundFont and Sampler **keep their internal polyphony** for now — SF_PLAY/SAMPLE_VOICE single-voice opcodes are future work
- Compile-time expansion is **deprecated** — a chord into a mono synth plays mono (root note only), like real hardware
- Old monophonic patterns (`pat("c4 e4 g4") |> osc(...)`) still work unchanged via SEQPAT_STEP
- Instrument functions take exactly `(freq, gate, vel)` — no extra params syntax

---

## 1. Target Syntax

### 1.1 Basic Usage

```akkado
// Define instrument as a function
fn lead(freq, gate, vel) =
    osc("saw", freq) |> lp(%, 2000 * adsr(gate)) |> % * vel

// Explicit polyphonic (8-voice)
pat("C4' Am7' G4'") |> poly(%, 8, lead) |> out(%, %)

// Explicit monophonic (last-note priority, retrigger)
pat("c4 e4 g4 b4") |> mono(%, lead) |> out(%, %)
```

### 1.2 SoundFont and Sampler (Future)

```akkado
// SoundFont as instrument (requires SF_PLAY opcode — future work)
pat("C4' Am7'") |> poly(%, 32, soundfont("piano.sf2")) |> out(%, %)

// Sampler as instrument (requires SAMPLE_VOICE opcode — future work)
pat("x _ x _") |> poly(%, 4, sample("kick.wav")) |> out(%, %)
```

> **Note**: `soundfont` exists in `builtins.hpp` as `Opcode::NOP` (handled specially by codegen). SF_PLAY and SAMPLE_VOICE single-voice opcodes are future phases — SoundFont and Sampler keep their internal voice allocators until then.

### 1.3 MIDI Input (Future)

```akkado
midi_in() |> poly(%, 16, lead) |> out(%, %)
```

### 1.4 Mono and Legato

```akkado
// mono = poly(1) with last-note priority and retrigger
pat("c4 e4 g4 b4") |> mono(%, lead) |> out(%, %)

// legato = mono without retrigger (freq glides, gate stays high)
pat("c4 e4 g4 b4") |> legato(%, lead) |> out(%, %)
```

**Mono voice count**: `mono()` compiles to `poly(1, ...)` — envelope release tail is cut when a new note arrives. This matches hardware mono synths. Users who want release overlap can use `poly(2, ...)` with `mode=mono`.

### 1.5 Effects After Polyphony

POLY outputs a mixed signal — effects chain naturally after it:

```akkado
fn pad(freq, gate, vel) =
    osc("saw", freq) |> lp(%, 1000) |> % * adsr(gate, 0.1, 0.2, 0.6, 0.5) * vel

pat("C4' Am7' G7' F4'")
|> poly(%, 8, pad)    // 8 voices, mixed to mono
|> reverb(%)          // shared reverb on the mix
|> out(%, %) * 0.3
```

### 1.6 Instrument Functions with External Parameters

Instrument functions take exactly `(freq, gate, vel)`. Additional parameters are captured via closure:

```akkado
cutoff = param("cutoff", 2000)
fn lead(freq, gate, vel) =
    osc("saw", freq) |> lp(%, cutoff * adsr(gate)) |> % * vel

pat("C4'") |> poly(%, 8, lead) |> out(%, %)
```

---

## 2. Architecture

### 2.1 Bytecode Layout

The POLY block brackets a body of instructions that execute once per active voice:

```
SEQPAT_QUERY    state_id=S           // Set up pattern events
POLY_BEGIN      rate=body_length     // Start poly block
                out=mix_buf
                in0=voice_freq_buf
                in1=voice_gate_buf
                in2=voice_vel_buf
                in3=voice_trig_buf
                in4=voice_out_buf
                state_id=P           // PolyAllocState

  // Body: executed once per active voice
  OSC_SAW       in0=voice_freq_buf   out=tmp0   state_id=A
  FILTER_SVF_LP in0=tmp0 in1=... in2=...  out=tmp1   state_id=B
  ENV_ADSR      in0=voice_gate_buf   out=tmp2   state_id=C
  MUL           in0=tmp1 in1=tmp2    out=voice_out_buf

POLY_END                             // End marker
OUTPUT          in0=mix_buf in1=mix_buf
```

Key points:
- `rate` field encodes body_length (0-255 instructions)
- Body instructions are ordinary opcodes — no special per-voice variants
- `voice_freq_buf` etc. are scratch buffers filled per-voice before each body iteration
- `voice_out_buf` is accumulated into `mix_buf` after each body iteration

### 2.2 VM Execution

Change `execute_program` from range-for to index-based:

```cpp
auto program = slot->program();
std::size_t ip = 0;
while (ip < program.size()) {
    if (program[ip].opcode == Opcode::POLY_BEGIN) {
        ip = execute_poly_block(program, ip);
    } else {
        execute(program[ip]);
        ++ip;
    }
}
```

`execute_poly_block` algorithm:
1. Read POLY_BEGIN instruction config
2. Get `PolyAllocState` from `state_id`
3. Convert events from linked `SequenceState` → NoteEvents (see 2.6)
4. Feed NoteEvents to voice allocator → update voice slots
5. Clear `mix_buf` to zero
6. For each active voice (0..max_voices-1):
   a. Fill `voice_freq/gate/vel/trig` buffers from voice slot data
   b. Set `state_pool_.state_id_xor = voice_index * 0x9E3779B9u + 1` (golden ratio salt)
   c. Execute body instructions (ip+1 to ip+body_length)
   d. Accumulate `voice_out_buf` into `mix_buf`
7. Reset `state_pool_.state_id_xor = 0`
8. Return `ip + body_length + 2` (past POLY_END)

### 2.3 State ID Isolation (XOR Approach)

Each voice needs isolated state (oscillator phase, filter memory, etc.). Instead of modifying instructions per-voice, add a transparent XOR offset to StatePool:

```cpp
// state_pool.hpp
class StatePool {
    std::uint32_t state_id_xor_ = 0;  // NEW
public:
    void set_state_id_xor(std::uint32_t xor_val) { state_id_xor_ = xor_val; }

    template<typename T>
    T& get_or_create(std::uint32_t state_id) {
        std::uint32_t effective_id = state_id ^ state_id_xor_;  // transparent
        // ...existing lookup with effective_id...
    }
};
```

Properties:
- **Zero cost** when no POLY active (XOR with 0 = identity)
- **No changes** to any existing opcode implementations
- **Nested POLY** works: save/restore xor, compose via XOR
- **Hot-swap** works: state_ids are deterministic for same voice index
- **Golden ratio salt** (`0x9E3779B9u`) provides perfect distribution, avoids hash collisions between voices

### 2.4 Instruction Encoding

```
POLY_BEGIN:
  rate        = body_length (0-255 instructions)
  out_buffer  = mix output buffer
  inputs[0]   = voice_freq_buf (body reads freq from here)
  inputs[1]   = voice_gate_buf
  inputs[2]   = voice_vel_buf
  inputs[3]   = voice_trig_buf
  inputs[4]   = voice_out_buf (body writes final audio here)
  state_id    = PolyAllocState hash

POLY_END:
  NOP marker (no data needed, just body terminator)
```

### 2.5 Buffer Reuse

Body temp buffers are reused across voice iterations — only one voice runs at a time (full BLOCK_SIZE). The `voice_out_buf` is accumulated into `mix_buf` after each iteration, then overwritten by the next voice. This keeps buffer usage minimal.

### 2.6 NoteEvent Abstraction

Pattern events use positional time representation (time-within-cycle), but voice allocation needs sample-accurate gate on/off timing. The `NoteEvent` struct bridges this gap and provides a unified interface for both pattern and future MIDI input:

```cpp
struct NoteEvent {
    float freq;                    // Hz
    float vel;                     // 0-1
    uint16_t event_index;          // source OutputEvent index
    uint16_t voice_index;          // which chord note (0-3)
    uint32_t gate_on_sample;       // sample offset within block for note-on
    uint32_t gate_off_sample;      // sample offset for note-off (BLOCK_SIZE if sustaining)
};
```

**Conversion paths:**
- **Pattern path**: `execute_poly_block` scans `OutputEvents` from the linked `SequenceState`, converts positional events to NoteEvents per block based on current beat position and samples-per-beat
- **Future MIDI path**: MIDI note-on/note-off messages convert to NoteEvents directly (sample offset from MIDI timestamp)

The POLY voice allocator consumes `NoteEvent*` + count regardless of source, keeping the voice allocation logic completely decoupled from the event source.

### 2.7 Constraints

- **SEQPAT_STEP/GATE/TYPE must NOT appear inside POLY body** — they mutate `SequenceState.current_index` (shared state), which would corrupt event tracking across voice iterations
- **POLY body opcodes must be stateless or use state_id** — the XOR isolation mechanism only works for state accessed via `StatePool::get_or_create(state_id)`. Opcodes that use global mutable state will not be isolated per-voice.
- **Body length limited to 255 instructions** — the `rate` field in `Instruction` is 8-bit (`std::uint8_t`), capping the body at 255 opcodes. This is sufficient for complex instrument patches.

---

## 3. PolyAllocState

```cpp
struct PolyAllocState {
    static constexpr uint16_t MAX_VOICES = 32;

    // Config (set by compiler via init_poly_state)
    uint32_t seq_state_id = 0;    // Linked SequenceState for events
    uint8_t max_voices = 8;
    uint8_t mode = 0;             // 0=poly, 1=mono, 2=legato
    uint8_t steal_strategy = 0;   // 0=oldest, 1=quietest

    struct Voice {
        float freq = 0.0f;
        float vel = 0.0f;
        float gate = 0.0f;
        bool active = false;
        bool releasing = false;
        uint32_t age = 0;
        int8_t note = -1;                 // MIDI note for voice stealing
        uint16_t event_index = 0xFFFF;    // which OutputEvent this voice is playing
        uint32_t cycle = 0;               // which cycle the event belongs to
    };
    Voice voices[MAX_VOICES] = {};

    // Voice allocation
    Voice* allocate(float freq, float vel);
    void release_by_event(uint16_t event_index, uint32_t cycle);

    // Event processing — consumes NoteEvents regardless of source
    void process_note_events(const NoteEvent* events, uint32_t count);

    // Per-block maintenance
    void tick();  // age++, clean released voices

    // Web UI state inspection
    std::string inspect_state_json() const;
};
```

### 3.1 Voice Allocation Strategy

**Poly mode** (mode=0):
1. Find first inactive slot -> assign note
2. If all slots active, steal by strategy:
   - `oldest` (default): steal voice with highest age
   - `quietest`: steal voice with lowest vel that is also releasing
3. On note-off: set `releasing = true`, keep `gate = 0` so envelope can release naturally

**Mono mode** (mode=1):
1. Always use voice slot 0
2. On new note: retrigger (gate 0->1 transition), update freq/vel
3. On note-off: only release if the released note matches current note (last-note priority)

**Legato mode** (mode=2):
1. Always use voice slot 0
2. On new note: update freq/vel but do NOT retrigger (gate stays 1)
3. On note-off: same as mono

### 3.2 Event Processing

`process_note_events` replaces the old `process_events(SequenceState&, ...)` — it operates on `NoteEvent` structs that have already been converted from whatever source (pattern OutputEvents, future MIDI):

```cpp
void PolyAllocState::process_note_events(const NoteEvent* events,
                                          uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const auto& ne = events[i];
        Voice* v = allocate(ne.freq, ne.vel);
        if (v) {
            v->freq = ne.freq;
            v->vel = ne.vel;
            v->gate = 1.0f;
            v->event_index = ne.event_index;
            // gate_on_sample / gate_off_sample used for per-sample accuracy
        }
    }

    // Release voices whose events have exited the time window.
    // Each voice tracks (event_index, cycle) — when the event's
    // duration has elapsed, call release_by_event().
    tick();  // age all voices
}
```

Voice release is **event-index-based** rather than frequency-based: each voice tracks which `(event_index, cycle)` it's playing. A voice is released when its event exits the current time window, avoiding the ambiguity of frequency-matching (which breaks with multiple voices at the same pitch).

### 3.3 Per-Sample Gate Accuracy

For trigger accuracy, the voice buffers are filled with per-sample precision using `NoteEvent.gate_on_sample` and `gate_off_sample`:

```cpp
// Fill voice_trig_buf: 1.0 at exact trigger sample, 0.0 elsewhere
// Fill voice_gate_buf: 0.0 before note-on, 1.0 from gate_on_sample onward
//                      1.0 before note-off, 0.0 from gate_off_sample onward
// This gives sub-block timing accuracy for envelopes
```

### 3.4 State Inspector

`PolyAllocState` must implement `inspect_state_json()` for the web UI state inspector (StateInspector panel). This allows live visualization of voice allocation, active voices, frequencies, and gate states during playback:

```cpp
std::string PolyAllocState::inspect_state_json() const {
    // Returns JSON with: max_voices, mode, steal_strategy,
    // and per-voice: freq, vel, gate, active, releasing, age, event_index
}
```

This follows the pattern of existing `inspect_state_json()` in `StatePool` (see `cedar/include/cedar/vm/state_pool.hpp`).

---

## 4. Compiler Integration

### 4.1 Compiling `poly(8, lead)`

When the compiler encounters `poly(8, instrument_fn)`:

1. **Emit SEQPAT_QUERY** for the upstream pattern (already exists)
2. **Allocate 5 scratch buffers**: voice_freq, voice_gate, voice_vel, voice_trig, voice_out
3. **Allocate mix output buffer**
4. **Emit POLY_BEGIN** with config (max_voices, mode, body_length)
5. **Compile instrument function body** with params mapped to voice buffers:
   - `freq` parameter -> reads from `voice_freq_buf`
   - `gate` parameter -> reads from `voice_gate_buf`
   - `vel` parameter -> reads from `voice_vel_buf`
6. **Emit POLY_END**
7. **Call `vm.init_poly_state(state_id, seq_state_id, max_voices, mode)`** to link event source

### 4.2 Instrument Function Convention

Instrument functions take exactly `(freq, gate, vel)`:

```akkado
fn lead(freq, gate, vel) =
    osc("saw", freq) |> lp(%, 2000 * adsr(gate)) |> % * vel
```

The compiler maps these positionally:
- Param 0 (`freq`) -> `voice_freq_buf`
- Param 1 (`gate`) -> `voice_gate_buf`
- Param 2 (`vel`) -> `voice_vel_buf`

Additional parameters use closure capture (see section 1.6).

### 4.3 `mono()` and `legato()` as Sugar

```akkado
mono(lead)   -> poly(1, lead)  with mode=1 (mono)
legato(lead) -> poly(1, lead)  with mode=2 (legato)
```

### 4.4 `soundfont()` and `sample()` as Instrument Factories (Future)

These will compile to special single-voice opcodes (SF_PLAY, SAMPLE_VOICE) wrapped in the standard instrument function convention:

```akkado
// This:
poly(32, soundfont("piano.sf2"))

// Will compile as if it were:
poly(32, fn(freq, gate, vel) = sf_play("piano.sf2", freq, gate, vel))
```

> **Note**: Until SF_PLAY and SAMPLE_VOICE opcodes are implemented, SoundFont and Sampler continue using their existing internal voice allocators. The `soundfont` builtin is registered in `builtins.hpp` as `Opcode::NOP` (handled specially by codegen).

---

## 5. What Stays vs What Changes

| Component | Status | Notes |
|-----------|--------|-------|
| Multi-buffer system for arrays | **Stays** | `[440, 550] \|> osc(%)` still works |
| SEQPAT_QUERY | **Stays** | POLY reads events from it |
| Sequence/Event structs | **Modified** | Add `velocity` field (Phase 0) |
| UGen auto-expansion from multi-buffer | **Stays for arrays** | Removed for polyphonic patterns |
| Per-voice SEQPAT_STEP/GATE/TYPE emission | **Removed** | POLY handles voice distribution |
| PolyphonicFields struct | **Removed** | No longer needed (currently in `akkado/include/akkado/codegen.hpp`) |
| SoundFont internal voice allocator | **Stays** | Future work (SF_PLAY single-voice opcode) |
| Sampler internal voice allocator | **Stays** | Future work (SAMPLE_VOICE single-voice opcode) |
| `$polyphony(n)` directive | **Stays** | Sets default for `poly()` |
| `spread()` function | **Stays** | Useful for array padding |
| Old monophonic patterns | **Stays** | `pat("c4 e4") \|> osc(...)` still works via SEQPAT_STEP |

---

## 6. MAX_STATES Consideration

Current limit: 256 state entries. With 8 voices and 6 stateful opcodes per voice = 48 states for one POLY block. Two POLY blocks = 96 states — already significant.

**Action**: May need to increase `MAX_STATES` to 512 or 1024.

File: `cedar/include/cedar/vm/state_pool.hpp` — `MAX_STATES` constant.

---

## 7. Implementation Phases

### Phase 0: Velocity in Events (Prerequisite)
**Goal**: Event and OutputEvent structs carry velocity data for POLY consumption

Files to modify:
- `cedar/include/cedar/opcodes/sequence.hpp` — add `float velocity` field to `Event` and `OutputEvent` structs
- `cedar/include/cedar/opcodes/sequencing.hpp` — update `OutputEvents::add()` to accept velocity, update `op_seqpat_step` to output actual velocity instead of hardcoded 1.0
- `akkado/src/mini_notation.cpp` — support velocity syntax in patterns (e.g., `c4:0.8` or `c4@0.8`)
- `akkado/src/codegen_patterns.cpp` — update `SequenceCompiler` to emit velocity from pattern events

**Test**: Verify existing patterns still compile and produce velocity=1.0 by default. Verify explicit velocity syntax outputs correct values.

### Phase 1: Cedar POLY Infrastructure
**Goal**: VM can execute POLY_BEGIN/POLY_END with hardcoded voice data

Files to modify:
- `cedar/include/cedar/vm/instruction.hpp` — add `POLY_BEGIN`, `POLY_END` opcodes
- `cedar/include/cedar/vm/state_pool.hpp` — add `state_id_xor_` field, modify `get_or_create`
- `cedar/include/cedar/opcodes/dsp_state.hpp` — add `PolyAllocState` struct (with `NoteEvent`), add to `DSPState` variant
- `cedar/src/vm/vm.hpp` — add `execute_poly_block` method, add `init_poly_state` method
- `cedar/src/vm/vm.cpp` — change `execute_program` to index-based loop, implement `execute_poly_block`
- `cedar/include/cedar/generated/opcode_metadata.hpp` — regenerate (run `bun run build:opcodes`)

**Test**: Python experiment with hardcoded bytecode — 3 oscillators at different frequencies via POLY.

### Phase 2: Akkado Compiler — `poly()` and `mono()`
**Goal**: `pat("C4'") |> poly(%, 8, synth_fn) |> out(%, %)` compiles and plays

Files to modify:
- `akkado/include/akkado/builtins.hpp` — add `poly` and `mono` builtin entries
- `akkado/src/codegen.cpp` — implement `handle_poly_call`, wire instrument function body compilation
- `akkado/include/akkado/codegen.hpp` — add poly-related state tracking
- `akkado/tests/test_codegen.cpp` — tests for POLY bytecode generation

Key logic:
- Detect pipe input is a pattern -> emit SEQPAT_QUERY, link to POLY
- Resolve instrument function -> inline body with voice buffer params
- Compute body_length, emit POLY_BEGIN/body/POLY_END

### Phase 3: Voice Allocation Logic
**Goal**: Proper event-driven voice allocation, stealing, mono, legato

Files to modify:
- `cedar/include/cedar/opcodes/dsp_state.hpp` — flesh out `PolyAllocState::process_note_events`, `allocate`, `release_by_event`
- `cedar/src/vm/vm.cpp` — integrate NoteEvent conversion and event processing into `execute_poly_block`

Features:
- NoteEvent conversion from OutputEvents in execute_poly_block
- Event-index-based voice release (not frequency-based)
- Voice stealing (oldest first, then quietest releasing)
- Mono mode: kill previous voice on new note
- Legato mode: update freq without retrigger
- Per-sample gate accuracy (trigger pulse at exact event time via NoteEvent.gate_on_sample)

### Phase 4: Deprecation — Remove Compile-Time Expansion
**Goal**: Clean up old implicit polyphony system

Files to modify:
- `akkado/src/codegen_patterns.cpp` — remove per-voice SEQPAT_STEP/GATE/TYPE emission
- `akkado/include/akkado/codegen.hpp` — remove `PolyphonicFields` struct
- `akkado/src/codegen.cpp` — remove PolyphonicFields usage
- Keep multi-buffer auto-expansion for arrays (`[440, 550] |> osc(%)` still works)

Behavior changes:
- Chord into mono synth -> plays root note only (like real hardware)
- Old monophonic patterns still work via SEQPAT_STEP (`pat("c4 e4 g4") |> osc(...)`)
- Implicit chord expansion (`pat("C4'") |> osc(...)` without `poly()`) -> error with migration message suggesting `poly()`

### Future: SF_PLAY Single-Voice SoundFont Opcode

Files to create/modify:
- `cedar/include/cedar/opcodes/soundfont.hpp` — add `SF_PLAY` opcode (single-voice SoundFont playback, extracted from SOUNDFONT_VOICE)
- `cedar/include/cedar/vm/instruction.hpp` — add `SF_PLAY` opcode
- `cedar/src/vm/vm.cpp` — register new opcode
- `akkado/include/akkado/builtins.hpp` — `soundfont()` as instrument factory
- `akkado/src/codegen.cpp` — `soundfont("file")` compiles to instrument-like callable

`SF_PLAY` = the inner loop of current SOUNDFONT_VOICE, minus voice allocation:
- Takes freq, gate, vel, preset as inputs
- Handles zone lookup, sample interpolation, per-voice filter, DAHDSR envelope
- Returns mono audio output

### Future: SAMPLE_VOICE Single-Voice Sampler Opcode

Files to create/modify:
- `cedar/include/cedar/opcodes/samplers.hpp` — add `SAMPLE_VOICE` opcode (single-voice, extracted from SAMPLE_PLAY)
- Same pattern as SF_PLAY

---

## 8. Verification

### 8.1 Per-Phase Testing

| Phase | Test | Method |
|-------|------|--------|
| 0 | Event velocity field populated correctly, default 1.0 | Unit test + Python experiment |
| 1 | 3 oscillators at different frequencies mix correctly | Python experiment — build POLY bytecode manually |
| 2 | `poly(8, fn(f,g,v) = osc("sin",f)*adsr(g))` produces correct instruction count | Akkado compiler test |
| 3 | Voice stealing works when exceeding max_voices; mono plays only last note | Audio integration test |
| 4 | Old `pat("C4'") \|> osc(...)` gives error with migration hint; `pat("c4 e4") \|> osc(...)` still works | Regression test suite |

### 8.2 Manual Testing

```akkado
// Basic polyphonic chord (verify 3 voices mix)
fn pad(f, g, v) = osc("saw", f) |> lp(%, 1000) |> % * adsr(g, 0.1, 0.2, 0.6, 0.5) * v
pat("C4' Am7' G7' F4'") |> poly(%, 8, pad) |> out(%, %) * 0.3

// Mono lead (verify only 1 voice at a time)
fn lead(f, g, v) = osc("saw", f) |> lp(%, 3000) |> % * adsr(g, 0.01, 0.1, 0.8, 0.2)
pat("c4 e4 g4 c5 g4 e4") |> mono(%, lead) |> out(%, %) * 0.5

// Old monophonic pattern (verify still works without poly())
pat("c4 e4 g4") |> osc("sin", %.freq) |> out(%, %)

// Voice stealing — exceed 4 voices with 7th chord
fn simple(f, g, v) = osc("sin", f) * adsr(g) * v
pat("Am7' CM9'") |> poly(%, 4, simple) |> out(%, %) * 0.3

// Closure capture for extra params
cutoff = param("cutoff", 2000)
fn filtered(f, g, v) = osc("saw", f) |> lp(%, cutoff * adsr(g)) |> % * v
pat("C4' Am7'") |> poly(%, 8, filtered) |> out(%, %) * 0.3
```

---

## 9. Summary

| Feature | Syntax | Phase |
|---------|--------|-------|
| Velocity in Event/OutputEvent | (struct change) | Phase 0 |
| NoteEvent abstraction | (internal) | Phase 1 |
| POLY_BEGIN/POLY_END opcodes | (bytecode) | Phase 1 |
| State ID XOR isolation | (transparent) | Phase 1 |
| `poly(n, instrument)` | `pat(...) \|> poly(%, 8, lead)` | Phase 2 |
| `mono(instrument)` | `pat(...) \|> mono(%, lead)` | Phase 2 |
| Voice allocation & stealing | (runtime) | Phase 3 |
| Mono/legato modes | `mono(lead)`, `legato(lead)` | Phase 3 |
| Event-index-based release | (runtime) | Phase 3 |
| Deprecate compile-time expansion | Migration errors | Phase 4 |
| SoundFont single-voice | `poly(32, soundfont("piano.sf2"))` | Future |
| Sampler single-voice | `poly(4, sample("kick.wav"))` | Future |

**Key design principles**:
1. **Explicit over implicit** — user chooses `poly(n, ...)` or `mono(...)`, never auto-expanded
2. **One allocator for everything** — oscillators, SoundFonts, samplers all use PolyAllocState
3. **Zero-cost abstraction** — XOR state isolation adds no overhead outside POLY blocks
4. **Hot-swap safe** — deterministic state IDs per voice index
5. **Composable** — instruments are ordinary functions, effects chain after POLY naturally
6. **Source-agnostic** — NoteEvent abstraction decouples voice allocation from event source (patterns today, MIDI tomorrow)

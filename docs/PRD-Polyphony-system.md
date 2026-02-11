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

---

## 1. Target Syntax

### 1.1 Basic Usage

```akkado
// Define instrument as a function
fn lead(freq, gate, vel) =
    osc("saw", freq) |> lp(%, 2000 * adsr(gate)) |> % * vel

// Explicit polyphonic (8-voice)
pat("C4' Am7' G4'") |> poly(8, lead) |> out(%, %)

// Explicit monophonic (last-note priority, retrigger)
pat("c4 e4 g4 b4") |> mono(lead) |> out(%, %)
```

### 1.2 SoundFont and Sampler

```akkado
// SoundFont as instrument
pat("C4' Am7'") |> poly(32, soundfont("piano.sf2")) |> out(%, %)

// Sampler as instrument
pat("x _ x _") |> poly(4, sample("kick.wav")) |> out(%, %)
```

### 1.3 MIDI Input (Future)

```akkado
midi_in() |> poly(16, lead) |> out(%, %)
```

### 1.4 Mono and Legato

```akkado
// mono = poly(1) with last-note priority and retrigger
pat("c4 e4 g4 b4") |> mono(lead) |> out(%, %)

// legato = mono without retrigger (freq glides, gate stays high)
pat("c4 e4 g4 b4") |> legato(lead) |> out(%, %)
```

### 1.5 Effects After Polyphony

POLY outputs a mixed signal — effects chain naturally after it:

```akkado
fn pad(freq, gate, vel) =
    osc("saw", freq) |> lp(%, 1000) |> % * adsr(gate, 0.1, 0.2, 0.6, 0.5) * vel

pat("C4' Am7' G7' F4'")
|> poly(8, pad)       // 8 voices, mixed to mono
|> reverb(%)          // shared reverb on the mix
|> out(%, %) * 0.3
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
3. Read events from linked `SequenceState` → update voice allocation
4. Clear `mix_buf` to zero
5. For each active voice (0..max_voices-1):
   a. Fill `voice_freq/gate/vel/trig` buffers from voice slot data
   b. Set `state_pool_.state_id_xor = voice_index * 0x9E3779B9u + 1` (golden ratio salt)
   c. Execute body instructions (ip+1 to ip+body_length)
   d. Accumulate `voice_out_buf` into `mix_buf`
6. Reset `state_pool_.state_id_xor = 0`
7. Return `ip + body_length + 2` (past POLY_END)

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
        int8_t note = -1;         // MIDI note for voice stealing
    };
    Voice voices[MAX_VOICES] = {};

    // Voice allocation
    Voice* allocate(float freq, float vel);
    void release_by_freq(float freq);

    // Event processing
    void process_events(const SequenceState& seq, float beat_pos, float samples_per_beat);

    // Per-block maintenance
    void tick();  // age++, clean released voices
};
```

### 3.1 Voice Allocation Strategy

**Poly mode** (mode=0):
1. Find first inactive slot → assign note
2. If all slots active, steal by strategy:
   - `oldest` (default): steal voice with highest age
   - `quietest`: steal voice with lowest vel that is also releasing
3. On note-off: set `releasing = true`, keep `gate = 0` so envelope can release naturally

**Mono mode** (mode=1):
1. Always use voice slot 0
2. On new note: retrigger (gate 0→1 transition), update freq/vel
3. On note-off: only release if the released note matches current note (last-note priority)

**Legato mode** (mode=2):
1. Always use voice slot 0
2. On new note: update freq/vel but do NOT retrigger (gate stays 1)
3. On note-off: same as mono

### 3.2 Event Processing

`process_events` reads from the linked `SequenceState`:

```cpp
void PolyAllocState::process_events(const SequenceState& seq,
                                     float beat_pos,
                                     float samples_per_beat) {
    // For each event in the current block:
    for (const auto& event : seq.current_events()) {
        if (event.is_note_on()) {
            Voice* v = allocate(event.freq, event.vel);
            if (v) {
                v->freq = event.freq;
                v->vel = event.vel;
                v->gate = 1.0f;
                v->note = event.note;
                // trig_sample = exact sample position within block
            }
        } else if (event.is_note_off()) {
            release_by_freq(event.freq);
        }
    }
    tick();  // age all voices
}
```

### 3.3 Per-Sample Gate Accuracy

For trigger accuracy, the voice buffers are filled with per-sample precision:

```cpp
// Fill voice_trig_buf: 1.0 at exact trigger sample, 0.0 elsewhere
// Fill voice_gate_buf: 0.0 before note-on, 1.0 from note-on sample onward
// This gives sub-block timing accuracy for envelopes
```

---

## 4. Compiler Integration

### 4.1 Compiling `poly(8, lead)`

When the compiler encounters `poly(8, instrument_fn)`:

1. **Emit SEQPAT_QUERY** for the upstream pattern (already exists)
2. **Allocate 5 scratch buffers**: voice_freq, voice_gate, voice_vel, voice_trig, voice_out
3. **Allocate mix output buffer**
4. **Emit POLY_BEGIN** with config (max_voices, mode, body_length)
5. **Compile instrument function body** with params mapped to voice buffers:
   - `freq` parameter → reads from `voice_freq_buf`
   - `gate` parameter → reads from `voice_gate_buf`
   - `vel` parameter → reads from `voice_vel_buf`
6. **Emit POLY_END**
7. **Call `vm.init_poly_state(state_id, seq_state_id, max_voices, mode)`** to link event source

### 4.2 Instrument Function Convention

Instrument functions take `(freq, gate, vel)` as their first three parameters:

```akkado
fn lead(freq, gate, vel) =
    osc("saw", freq) |> lp(%, 2000 * adsr(gate)) |> % * vel
```

The compiler maps these positionally:
- Param 0 (`freq`) → `voice_freq_buf`
- Param 1 (`gate`) → `voice_gate_buf`
- Param 2 (`vel`) → `voice_vel_buf`

Additional parameters are allowed and compiled normally:

```akkado
fn lead(freq, gate, vel, cutoff) =
    osc("saw", freq) |> lp(%, cutoff * adsr(gate)) |> % * vel

pat("C4'") |> poly(8, lead(%, %, %, param("cutoff", 2000))) |> out(%, %)
```

### 4.3 `mono()` and `legato()` as Sugar

```akkado
mono(lead)   → poly(1, lead)  with mode=1 (mono)
legato(lead) → poly(1, lead)  with mode=2 (legato)
```

### 4.4 `soundfont()` and `sample()` as Instrument Factories

These compile to special single-voice opcodes (SF_PLAY, SAMPLE_VOICE) wrapped in the standard instrument function convention:

```akkado
// This:
poly(32, soundfont("piano.sf2"))

// Compiles as if it were:
poly(32, fn(freq, gate, vel) = sf_play("piano.sf2", freq, gate, vel))
```

---

## 5. What Stays vs What Changes

| Component | Status | Notes |
|-----------|--------|-------|
| Multi-buffer system for arrays | **Stays** | `[440, 550] \|> osc(%)` still works |
| SEQPAT_QUERY (pattern event setup) | **Stays** | POLY reads events from it |
| Sequence/Event structs | **Stays** | Event format unchanged |
| UGen auto-expansion from multi-buffer | **Stays for arrays** | Removed for polyphonic patterns |
| Per-voice SEQPAT_STEP/GATE/TYPE emission | **Removed** | POLY handles voice distribution |
| PolyphonicFields struct | **Removed** | No longer needed |
| SoundFont internal voice allocator | **Removed** | Replaced by POLY + SF_PLAY |
| Sampler internal voice allocator | **Removed** | Replaced by POLY + SAMPLE_VOICE |
| `$polyphony(n)` directive | **Stays** | Sets default for `poly()` |
| `spread()` function | **Stays** | Still useful for array padding |

---

## 6. MAX_STATES Consideration

Current limit: 256 state entries. With 8 voices and 6 stateful opcodes per voice = 48 states for one POLY block. Two POLY blocks = 96 states — already significant.

**Action**: May need to increase `MAX_STATES` to 512 or 1024.

File: `cedar/include/cedar/vm/state_pool.hpp` — `MAX_STATES` constant.

---

## 7. Implementation Phases

### Phase 1: Cedar POLY Infrastructure
**Goal**: VM can execute POLY_BEGIN/POLY_END with hardcoded voice data

Files to modify:
- `cedar/include/cedar/vm/instruction.hpp` — add `POLY_BEGIN`, `POLY_END` opcodes
- `cedar/include/cedar/vm/state_pool.hpp` — add `state_id_xor_` field, modify `get_or_create`
- `cedar/include/cedar/opcodes/dsp_state.hpp` — add `PolyAllocState` struct, add to `DSPState` variant
- `cedar/src/vm/vm.hpp` — add `execute_poly_block` method, add `init_poly_state` method
- `cedar/src/vm/vm.cpp` — change `execute_program` to index-based loop, implement `execute_poly_block`
- `cedar/include/cedar/generated/opcode_metadata.hpp` — regenerate (run `bun run build:opcodes`)

**Test**: Python experiment with hardcoded bytecode — 3 oscillators at different frequencies via POLY.

### Phase 2: Akkado Compiler — `poly()` and `mono()`
**Goal**: `pat("C4'") |> poly(8, synth_fn) |> out(%, %)` compiles and plays

Files to modify:
- `akkado/include/akkado/builtins.hpp` — add `poly` and `mono` builtin entries
- `akkado/src/codegen.cpp` — implement `handle_poly_call`, wire instrument function body compilation
- `akkado/include/akkado/codegen.hpp` — add poly-related state tracking
- `akkado/tests/test_codegen.cpp` — tests for POLY bytecode generation

Key logic:
- Detect pipe input is a pattern → emit SEQPAT_QUERY, link to POLY
- Resolve instrument function → inline body with voice buffer params
- Compute body_length, emit POLY_BEGIN/body/POLY_END

### Phase 3: Voice Allocation Logic
**Goal**: Proper event-driven voice allocation, stealing, mono, legato

Files to modify:
- `cedar/include/cedar/opcodes/dsp_state.hpp` — flesh out `PolyAllocState::process_events`, `allocate`, `release_by_freq`
- `cedar/src/vm/vm.cpp` — integrate event processing into `execute_poly_block`

Features:
- Event-driven allocation from SequenceState events
- Voice stealing (oldest first, then quietest releasing)
- Mono mode: kill previous voice on new note
- Legato mode: update freq without retrigger
- Per-sample gate accuracy (trigger pulse at exact event time)

### Phase 4: SoundFont Single-Voice Opcode
**Goal**: `poly(32, soundfont("piano.sf2"))` works

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

### Phase 5: Sampler Single-Voice Opcode
**Goal**: `poly(4, sample("kick.wav"))` works

Files to create/modify:
- `cedar/include/cedar/opcodes/samplers.hpp` — add `SAMPLE_VOICE` opcode (single-voice, extracted from SAMPLE_PLAY)
- Same pattern as Phase 4

### Phase 6: Deprecate Old System & Clean Up
- Remove per-voice SEQPAT_STEP/GATE/TYPE emission from compiler (keep SEQPAT_QUERY)
- Keep multi-buffer auto-expansion for arrays (`[440, 550] |> osc(%)` still works)
- Remove PolyphonicFields system from codegen
- Deprecate SOUNDFONT_VOICE, old SAMPLE_PLAY multi-voice behavior
- Update docs
- Error on old implicit polyphony patterns with helpful migration message

---

## 8. Verification

### 8.1 Per-Phase Testing

| Phase | Test | Method |
|-------|------|--------|
| 1 | 3 oscillators at different frequencies mix correctly | Python experiment — build POLY bytecode manually |
| 2 | `poly(8, fn(f,g,v) = osc("sin",f)*adsr(g))` produces correct instruction count | Akkado compiler test |
| 3 | Voice stealing works when exceeding max_voices; mono plays only last note | Audio integration test |
| 4 | `poly(8, soundfont("piano.sf2"))` matches audio quality of old SOUNDFONT_VOICE | A/B audio comparison |
| 5 | `poly(4, sample("kick.wav"))` triggers samples correctly | Audio integration test |
| 6 | Old syntax errors out with helpful message; all tests pass | Regression test suite |

### 8.2 Manual Testing

```akkado
// Basic polyphonic chord (verify 3 voices mix)
fn pad(f, g, v) = osc("saw", f) |> lp(%, 1000) |> % * adsr(g, 0.1, 0.2, 0.6, 0.5) * v
pat("C4' Am7' G7' F4'") |> poly(8, pad) |> out(%, %) * 0.3

// Mono lead (verify only 1 voice at a time)
fn lead(f, g, v) = osc("saw", f) |> lp(%, 3000) |> % * adsr(g, 0.01, 0.1, 0.8, 0.2)
pat("c4 e4 g4 c5 g4 e4") |> mono(lead) |> out(%, %) * 0.5

// SoundFont polyphonic
pat("C4' Am7'") |> poly(32, soundfont("piano.sf2")) |> out(%, %)

// Voice stealing — exceed 4 voices with 7th chord
fn simple(f, g, v) = osc("sin", f) * adsr(g) * v
pat("Am7' CM9'") |> poly(4, simple) |> out(%, %) * 0.3
```

---

## 9. Summary

| Feature | Syntax | Phase |
|---------|--------|-------|
| POLY_BEGIN/POLY_END opcodes | (bytecode) | Phase 1 |
| State ID XOR isolation | (transparent) | Phase 1 |
| `poly(n, instrument)` | `pat(...) \|> poly(8, lead)` | Phase 2 |
| `mono(instrument)` | `pat(...) \|> mono(lead)` | Phase 2 |
| Voice allocation & stealing | (runtime) | Phase 3 |
| Mono/legato modes | `mono(lead)`, `legato(lead)` | Phase 3 |
| SoundFont single-voice | `poly(32, soundfont("piano.sf2"))` | Phase 4 |
| Sampler single-voice | `poly(4, sample("kick.wav"))` | Phase 5 |
| Deprecate old system | Migration errors | Phase 6 |

**Key design principles**:
1. **Explicit over implicit** — user chooses `poly(n, ...)` or `mono(...)`, never auto-expanded
2. **One allocator for everything** — oscillators, SoundFonts, samplers all use PolyAllocState
3. **Zero-cost abstraction** — XOR state isolation adds no overhead outside POLY blocks
4. **Hot-swap safe** — deterministic state IDs per voice index
5. **Composable** — instruments are ordinary functions, effects chain after POLY naturally

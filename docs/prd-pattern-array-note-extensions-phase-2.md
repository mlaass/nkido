> **Status: NOT STARTED** — Follow-up to `prd-pattern-array-note-extensions.md`. Closes the four unmet goals from `docs/audits/prd-pattern-array-note-extensions_audit_2026-04-24.md`: time/structure modifiers (Phase 7), algorithmic generators (Phase 8), voicing system (Phase 4), and extended note properties (Phase 5).

# PRD: Strudel-Style Pattern System Extensions — Phase 2

## 1. Executive Summary

The original `prd-pattern-array-note-extensions.md` shipped its foundational layers — arrays, `map()`, the chord system, polymeter, and dot-call desugaring — and is in steady use. The 2026-04-24 audit identified four goals that remain unmet:

1. **Phase 7 — Time & structure modifiers**: 12 transforms (`early`, `late`, `palindrome`, `iter`, `iterBack`, `ply`, `linger`, `zoom`, `compress`, `segment`, `swing`, `swingBy`).
2. **Phase 8 — Algorithmic generators**: `run`, `binary`, `binaryN`. (`euclid` already ships.)
3. **Phase 4 — Voicing system**: `anchor`, `mode`, voice leading, `addVoicings`.
4. **Phase 5 — Extended note properties**: `bend`, `aftertouch`, `dur` fields and transforms.

Phase 2 of this PRD ships all four. It does **not** revisit string-as-pattern auto-parsing — that question is closed by the existing `p"..."` prefix and by sampler-vs-note autodetection in the codegen. It does not add new opcodes except for one targeted change to `SEQPAT_STEP` to expose a runtime cycle counter for `iter`/`iterBack`/(optionally) `palindrome`.

### Key Design Decisions

- **Dot-call is free for everything.** Method chaining is pure desugaring at the AST level. No new builtin needs special treatment to gain `.method()` syntax — register the function and chaining works.
- **Voicing is a pattern transform, not a primitive.** `chord("Am C G").anchor("c4").mode("below")` desugars to nested function calls; voicings are computed at compile time from chord intervals + anchor + mode using a greedy nearest-voicing algorithm.
- **`compress` becomes a pattern transform.** The existing `compress` → `comp` alias on the audio compressor is removed to free the name for the Strudel-canonical pattern transform. Users who want the compressor write `comp(...)` or `compressor(...)`.
- **`iter`/`iterBack` get one new runtime opcode field.** `SEQPAT_STEP` gains a cycle counter so `iter` can advance its starting subdivision per cycle without exploding event counts at compile time.
- **`PatternEvent` gains two fixed fields and one extensibility bag.** `bend` and `aftertouch` join `velocity` as first-class fields; arbitrary user properties go in a `std::vector<std::pair<std::string, float>>` that is empty by default (zero overhead when unused).
- **Mini-notation gains a record-style suffix.** `c4{vel:0.8, bend:0.2}` is parsed as a note with named properties. The existing positional `c4:0.8` form keeps working as a velocity shorthand.

---

## 2. Problem Statement

The audit summary:

> Goals met: 4 of 8. Phases 1, 3, 6 fully shipped. Phase 2 resolved via dot-call desugaring. Phases 4, 5, 7, 8 incomplete.

Specifically:

| Area | Audit finding |
|------|---------------|
| Phase 7 transforms | 0 of 12 implemented. PRD listed it "IN PROGRESS" but no work in tree. |
| Phase 8 generators | Only `euclid` present. `run`, `binary`, `binaryN` absent from builtins and codegen. |
| Phase 4 voicing | No `voicing.hpp`/`voicing.cpp`. No `anchor`/`mode`/`addVoicings`. |
| Phase 5 note props | `velocity` real; `bend`, `aftertouch`, `dur` overrides absent from `PatternEvent` and unsurfaced. |

The user-visible consequence: idiomatic Strudel patterns that lean on `early`, `palindrome`, `ply`, `swing`, `run`, etc. do not transfer to Akkado. Composers reach for these constantly in live coding; their absence is the most common rough edge in the pattern system today.

---

## 3. Goals and Non-Goals

### Goals

1. Ship all 12 time/structure modifiers from the original PRD's Phase 7 table.
2. Ship `run(n)`, `binary(n)`, `binaryN(n, bits)` as compile-time pattern constructors.
3. Ship voicing: `anchor`, `mode`, voice leading, `addVoicings` with built-in dictionaries.
4. Extend `PatternEvent` with `bend`, `aftertouch`, and a `properties` bag, plus `.bend()`, `.aftertouch()`, `.dur()` transforms.
5. Add record-style mini-notation suffix syntax: `c4{vel:0.8, bend:0.2}`.
6. Per-modifier unit tests; golden MIDI-note assertions for voicing.
7. Resolve the `compress` naming collision by removing the `compress` → `comp` audio-compressor alias.

### Non-Goals

- **String-as-pattern auto-parsing of bare strings.** Already addressed via `p"..."` prefix and sampler-vs-note autodetection in `codegen_patterns.cpp:337,731`. Bare strings remain bare strings outside pattern slots.
- **MIDI export of `bend` / `aftertouch`.** Phase 2 plumbs the values through; consuming them in a MIDI-out path is a separate PRD.
- **`inside` / `outside` higher-order transforms.** Listed as deferred in the original PRD §8; remains deferred.
- **Optimal-path voice leading (DP).** Greedy nearest-voicing only.
- **New audio opcodes.** Only `SEQPAT_STEP` gains a cycle-counter field; everything else is compile-time event-list manipulation.

---

## 4. Target Syntax

### 4.1 Time & Structure Modifiers

```akkado
// Functional and dot-call forms work identically (existing desugaring)
early(pat("c4 e4 g4"), 0.25)
pat("c4 e4 g4").early(0.25)

// Time shifts (wrap around cycle)
pat("bd sd hh cp").early(0.125)   // shift earlier by 1/8 cycle
pat("bd sd hh cp").late(0.125)    // shift later by 1/8 cycle

// Reversal modifiers
pat("c4 e4 g4 b4").palindrome()   // forward, then reverse, every 2 cycles

// Subdivision-based
pat("c4 e4 g4 b4").iter(4)        // start subdivision rotates per cycle
pat("c4 e4 g4 b4").iterBack(4)    // same, opposite direction

// Density
pat("bd sd").ply(3)               // each event repeats 3x in its slot
pat("c4 e4 g4 b4").linger(0.5)    // keep first half, repeat to fill cycle

// Time-window
pat("bd sd hh cp").zoom(0.25, 0.75)   // play middle 50%, stretched to fill cycle
pat("bd sd hh cp").compress(0.25, 0.75) // squash whole pattern into [0.25, 0.75)

// Discretization
pat("c4 e4 g4 b4").segment(8)    // sample at 8 evenly-spaced points

// Swing
pat("bd hh sd hh").swing()        // shorthand for swingBy(1/3, 4)
pat("bd hh sd hh").swing(8)       // 8-slice swing
pat("bd hh sd hh").swingBy(0.4, 4) // custom delay amount
```

### 4.2 Algorithmic Generators

```akkado
// run(n): integer sequence 0..n-1 as evenly-spaced events
run(8)                            // 0,1,2,3,4,5,6,7
run(8) |> mtof(% + 60) |> osc("saw", %)   // ascending chromatic

// binary(n): trigger pattern from binary representation of n
binary(0b1010)                    // 4 steps: 1, 0, 1, 0
binary(170) |> sampler(%, "kick") // 170 = 0b10101010, 8 steps

// binaryN(n, bits): zero-padded fixed-width
binaryN(5, 8)                     // 0b00000101 = 0,0,0,0,0,1,0,1

// Generators chain via dot-call (free, via desugaring)
run(16).fast(2).rev() |> mtof(% + 48) |> osc("sin", %)
binary(0b11010010).slow(2).ply(2) |> sampler(%, "hh")
```

### 4.3 Voicing

```akkado
// Built-in voicing modes
chord("Am C G F").anchor("c4").mode("below")   // all notes below c4
chord("Am C G F").anchor("c5").mode("above")   // all notes above c5
chord("Am C G F").anchor("c4").mode("duck")    // closest avoiding c4
chord("Am C G F").anchor("c4").mode("root")    // root in bass, rest near c4

// Composes with audio
chord("Am C G F")
  .anchor("c4")
  .mode("below")
  |> mtof(%)
  |> poly(%, lead, 4)
  |> out(%, %)

// Voice leading is automatic when mode is set (greedy nearest)
// Each chord change picks the inversion minimizing total semitone movement.

// Built-in voicing dictionary
chord("Am").voicing("drop2")   // standard drop-2 voicing
chord("Am").voicing("open")    // open position
chord("Am").voicing("closed")  // close position

// Custom voicing dictionaries
addVoicings("piano-jazz", {
  "M":  [0, 4, 7, 11, 14],   // intervals from root
  "m":  [0, 3, 7, 10, 14],
  "7":  [0, 4, 10, 13, 17]
})
chord("CM Am7 Dm7 G7").voicing("piano-jazz")
```

### 4.4 Extended Note Properties

```akkado
// Transforms (mirror velocity() pattern)
note("c4 e4 g4").bend("<0 0.5 -0.5>")
note("c4 e4 g4").aftertouch(0.7)
note("c4 e4 g4").dur(0.5)         // override per-event duration

// Mini-notation record suffix (new)
pat("c4{vel:0.8, bend:0.2} e4{vel:1.0} g4{vel:0.5, bend:-0.3}")

// Custom user properties via the same record suffix
pat("c4{vel:0.8, cutoff:0.3} e4{cutoff:0.7}")
// The `cutoff` value lands in PatternEvent.properties as ("cutoff", 0.3).
// Reachable from synth code via record field access on the bound event:
pat("c4{cutoff:0.3} e4{cutoff:0.7}") as e
  |> osc("saw", e.freq)
  |> lp(%, 200 + e.cutoff * 4000)
  |> out(%, %)

// Existing positional shorthand still works
pat("c4:0.8 e4:1.0")              // colon = velocity (unchanged)
```

---

## 5. Architecture

### 5.1 Time & Structure Modifiers — Implementation Pattern

All 12 transforms follow the existing `slow`/`fast`/`rev` pattern (see `akkado/src/codegen_patterns.cpp:1618` `compile_pattern_for_transform`):

1. Register in `akkado/include/akkado/builtins.hpp` with `Opcode::NOP`.
2. Add to `is_pattern_call()` switch (`akkado/src/codegen_patterns.cpp:1585`).
3. Add to the transform-recognition switch in `compile_pattern_for_transform()` (`:1675`).
4. Add transform logic that mutates `out_events` and/or `out_cycle_length`.
5. Add a `handle_<name>_call()` dispatch handler in `codegen_patterns.cpp` and register it in `codegen.cpp`.

**Per-transform compile-time semantics:**

| Transform | Implementation |
|-----------|----------------|
| `early(pat, n)` | For each `event.time`: `event.time = (event.time - n + 1) mod 1`. |
| `late(pat, n)` | For each `event.time`: `event.time = (event.time + n) mod 1`. |
| `palindrome(pat)` | Double `out_cycle_length`. Append a reversed copy of all events with `event.time' = 1 + (1 - event.time - event.duration)`. Pure compile-time. |
| `ply(pat, n)` | For each event of duration `d` at time `t`: replace with `n` events of duration `d/n` at times `t, t+d/n, ..., t+(n-1)d/n`. |
| `linger(pat, frac)` | Drop events with `time >= frac`. Scale remaining events' time and duration by `1/frac`. Tile to fill cycle by appending shifted copies. |
| `zoom(pat, start, end)` | Drop events outside `[start, end)`. Map remaining: `t' = (t - start) / (end - start)`, `d' = d / (end - start)`. |
| `compress(pat, start, end)` | Map all event times: `t' = start + t * (end - start)`, `d' = d * (end - start)`. |
| `segment(pat, n)` | At each of `n` evenly-spaced sample points, find the active event; emit a new event of duration `1/n` carrying that event's value. |
| `swing(pat, n)` | Equivalent to `swingBy(pat, 1/3, n)`. |
| `swingBy(pat, amount, n)` | Divide cycle into `n` slices; for each slice, events whose offset within the slice is `>= 0.5` get `time += amount * (1/(2n))`. |
| `iter(pat, n)` | **Runtime.** See §5.2. |
| `iterBack(pat, n)` | **Runtime.** See §5.2. |

Defaults: `swing(pat, n=4)`, `swingBy(pat, amount, n=4)`.

### 5.2 Runtime Cycle Counter for `iter` / `iterBack`

`iter(pat, n)` divides the pattern into `n` parts and on each cycle starts playback from a different part — `[0..n-1, 1..n-1+0, 2..n-1+0+1, ...]`. This requires runtime cycle awareness.

**Approach:** Add a `cycle_count` field to `SEQ_STEP_STATE` (the state struct read by `SEQPAT_STEP`) and a per-event `start_subdivision` rotation index applied at query time.

| Field | Location | Purpose |
|-------|----------|---------|
| `std::uint32_t cycle_index` | `cedar::SeqStepState` | Increments at each cycle wrap. |
| `std::uint8_t iter_n` | `cedar::SeqStepState` | Number of subdivisions; 0 = no iter. |
| `std::int8_t iter_dir` | `cedar::SeqStepState` | +1 for `iter`, -1 for `iterBack`, 0 for none. |

**At query time:** if `iter_n > 0`, the effective offset into the event list is `(cycle_index * iter_dir) mod iter_n`, applied as a rotation of which subdivision plays first. Compile-time still emits the unrotated event list; the rotation is purely a query-time index transform.

**Codegen:** `iter` and `iterBack` are compile-time *configurators* — they set `iter_n` and `iter_dir` on the emitted `SeqInit` payload. They do not multiply event count.

### 5.3 Algorithmic Generators

Generators are pattern *constructors* — they emit a `PatternEventStream` at compile time and then go through the same `SEQPAT_QUERY`/`SEQPAT_STEP` pipeline as any other pattern.

| Function | Compile-time output |
|----------|---------------------|
| `run(n)` | `n` events at times `0, 1/n, 2/n, ..., (n-1)/n`, each carrying value `0, 1, 2, ...` (n-1) and duration `1/n`. |
| `binary(n)` | `bits = floor(log2(n)) + 1` events. Event `i` is a trigger if bit `i` of `n` is set, else a rest. |
| `binaryN(n, bits)` | Same as `binary` but exactly `bits` events; high-order zeros emit rests. |

Each gets a `handle_run_call`/`handle_binary_call`/`handle_binaryN_call` in `codegen_patterns.cpp` that builds the event stream directly and emits the standard SEQPAT instruction sequence.

**Dot-call composition** is automatic: `run(8).fast(2).rev()` desugars to `rev(fast(run(8), 2))` at the AST level (existing analyzer behavior). No special generator-side support needed.

### 5.4 Voicing System

**Files to create:**
- `akkado/include/akkado/voicing.hpp` — public API
- `akkado/src/voicing.cpp` — algorithm implementation

**Public API:**

```cpp
namespace akkado::voicing {

enum class Mode { Below, Above, Duck, Root };

struct VoicingDict {
    std::unordered_map<std::string, std::vector<int>> qualities;
};

// Apply voicing to a chord progression. Returns one MIDI-note vector per chord,
// each already voice-led to minimize movement from the previous.
std::vector<std::vector<int>> voice_chords(
    const std::vector<ChordEventData>& chords,
    int anchor_midi,
    Mode mode,
    const VoicingDict* dict = nullptr  // nullptr = built-in voicings
);

// Global registration (Phase 5 of original PRD §8 carryover)
void register_voicing(std::string_view name, VoicingDict dict);
const VoicingDict* lookup_voicing(std::string_view name);

} // namespace akkado::voicing
```

**Algorithm — greedy nearest voicing per transition:**

1. Generate candidate inversions for each chord (root position + N-1 inversions).
2. For each candidate, apply `mode` constraint:
   - `below`: shift by octaves until all notes ≤ anchor.
   - `above`: shift by octaves until all notes ≥ anchor.
   - `duck`: pick the inversion+octave with smallest sum of `|note - anchor|` that does not include `anchor` itself.
   - `root`: keep root note as-is in bass octave; voice remaining notes near anchor.
3. For chord *i+1*, pick the candidate that minimizes total semitone distance from chord *i*'s chosen voicing.

Built-in voicing dictionaries available out of the box: `"close"`, `"open"`, `"drop2"`, `"drop3"`. Users register custom ones via `addVoicings()`.

**Codegen integration:** `anchor`, `mode`, `voicing` are pattern transforms (NOP builtins). `compile_pattern_for_transform()` recognizes them as "voicing transforms" — when present on a chord pattern, the chord events are routed through `voice_chords()` before being emitted as ordinary multi-value pattern events.

### 5.5 Extended Note Properties

**`PatternEvent` changes** (`akkado/include/akkado/pattern_event.hpp:34`):

```cpp
struct PatternEvent {
    PatternEventType type = PatternEventType::Rest;

    // Timing
    float time = 0.0f;
    float duration = 1.0f;
    bool dur_override = false;   // NEW: true if .dur() set duration explicitly

    // Dynamics
    float velocity = 1.0f;
    float chance = 1.0f;
    float bend = 0.0f;           // NEW: pitch bend, -1..1
    float aftertouch = 0.0f;     // NEW: aftertouch, 0..1

    // Existing fields...
    std::uint8_t midi_note = 60;
    // ...

    // NEW: extensible custom properties
    std::vector<std::pair<std::string, float>> properties;
};
```

**Transforms** mirror `velocity()`:

| Transform | Effect |
|-----------|--------|
| `bend(pat, value_or_pat)` | Sets `event.bend` on each event. Pattern arg cycles values like `velocity()` does. |
| `aftertouch(pat, value_or_pat)` | Sets `event.aftertouch`. |
| `dur(pat, value_or_pat)` | Sets `event.duration` and `event.dur_override = true`. |

All three follow the existing handler pattern in `codegen_patterns.cpp` and use `compile_pattern_for_transform()` recursion.

**Property bag access:** Custom properties from `c4{cutoff:0.3}` land in `event.properties`. They are exposed at codegen time via the existing pipe-bind mechanism — `pat("c4{cutoff:0.3}") as e |> ... e.cutoff` resolves `e.cutoff` by looking up `"cutoff"` in the event's `properties` vector at compile time.

### 5.6 Mini-Notation Record Suffix

**Lexer** (`akkado/src/mini_lexer.cpp`): when a `{` follows a note token, switch into a "note record" sub-mode that lexes `key:value` pairs separated by `,` until `}`.

**Parser** (`akkado/src/mini_parser.cpp`): a `MiniNote` AST node gains an optional `properties` field (vector of `(name, value)` pairs).

**Evaluator** (`akkado/src/pattern_eval.cpp`): when emitting a `PatternEvent`, copy `properties` from the AST node. Recognized names (`vel`, `bend`, `aftertouch`, `dur`) populate the corresponding fixed fields; unrecognized names go into `event.properties`.

**Backwards compatibility:** the existing positional `c4:0.8` (velocity shorthand) is unchanged. Lexer disambiguates: `:` after a note → positional, `{` after a note → record.

### 5.7 `compress` Naming Resolution

**Decision:** Remove the `compress` → `comp` alias from `BUILTIN_ALIASES` (`akkado/include/akkado/builtins.hpp:983`). `compress` becomes the pattern transform exclusively. Audio-side users write `comp(...)` or `compressor(...)` — both still aliased to the audio compressor.

**Migration impact:** any user code calling `compress(signal, ratio, ...)` for audio must rename to `comp(...)`. This is a breaking change; we accept it because (a) `comp`/`compressor` aliases keep the audio compressor reachable, (b) the pattern transform is more frequently needed than a third spelling of the compressor, and (c) Strudel uses `compress` for the time transform exclusively.

---

## 6. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `pat()`, `chord()`, `seq()`, `timeline()` | **Stays** | Constructors unchanged. |
| Existing transforms (`slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant`, `tune`) | **Stays** | Implementation pattern unchanged; new transforms follow the same shape. |
| `is_pattern_call()` switch | **Modified** | Add 12 new transform names + 3 new constructors. |
| `compile_pattern_for_transform()` | **Modified** | Add 12 new event-list rewrite branches; `iter`/`iterBack` set state instead of rewriting events. |
| `PatternEvent` struct | **Modified** | Add `bend`, `aftertouch`, `dur_override`, `properties`. |
| `cedar::SeqStepState` | **Modified** | Add `cycle_index`, `iter_n`, `iter_dir`. |
| `BUILTIN_ALIASES` (`compress` → `comp`) | **Removed** | Frees `compress` for the pattern transform. |
| `BUILTINS` map | **Modified** | Add 12 transform entries + 3 generator entries + 5 voicing entries (`anchor`, `mode`, `voicing`, `addVoicings`, plus 3 note-prop transforms `bend`, `aftertouch`, `dur`). |
| Mini-notation lexer/parser | **Modified** | Add record-suffix sub-mode after notes. |
| `voicing.hpp` / `voicing.cpp` | **New** | Voicing algorithms + dictionary registry. |
| Audio compressor implementation | **Stays** | `comp` and `compressor` aliases unchanged. |
| Existing tests | **Stays** | All current behavior preserved (except removed `compress` alias — must update any test using it). |

---

## 7. File-Level Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/builtins.hpp` | Add 23 new entries (`early`, `late`, `palindrome`, `iter`, `iterBack`, `ply`, `linger`, `zoom`, `compress`, `segment`, `swing`, `swingBy`, `run`, `binary`, `binaryN`, `anchor`, `mode`, `voicing`, `addVoicings`, `bend`, `aftertouch`, `dur`). Remove `{"compress", "comp"}` from `BUILTIN_ALIASES`. |
| `akkado/include/akkado/codegen.hpp` | Declare 23 new `handle_*_call` functions. |
| `akkado/src/codegen.cpp` | Register 23 new dispatch entries in the call-handler map. |
| `akkado/src/codegen_patterns.cpp` | Implement 23 handlers + extend `compile_pattern_for_transform()` with new transform branches + extend `is_pattern_call()` recognition. |
| `akkado/include/akkado/pattern_event.hpp` | Add `bend`, `aftertouch`, `dur_override`, `properties` fields to `PatternEvent`. |
| `akkado/src/pattern_eval.cpp` | Copy record-suffix properties from AST into `PatternEvent`. |
| `akkado/include/akkado/mini_token.hpp` | Token-class changes for record-suffix lexing if needed (likely reuse existing brace tokens). |
| `akkado/src/mini_lexer.cpp` | Add note-record sub-mode (lexes `{key:value,...}` after a note). |
| `akkado/include/akkado/ast.hpp` | Add optional `properties` field to `MiniNote` data. |
| `akkado/src/mini_parser.cpp` | Parse record suffix into `MiniNote.properties`. |
| `cedar/include/cedar/state/seq_step_state.hpp` (or equivalent) | Add `cycle_index`, `iter_n`, `iter_dir` fields. |
| `cedar/include/cedar/opcodes/seqpat_step.hpp` (or equivalent) | Increment `cycle_index` at cycle wrap; apply iter rotation at query time. |
| `akkado/include/akkado/voicing.hpp` | **New.** Public voicing API. |
| `akkado/src/voicing.cpp` | **New.** Greedy voice-leading algorithm + built-in dictionaries + `addVoicings` registry. |
| `akkado/tests/test_pattern_transforms.cpp` (new or existing) | One unit test per new transform. |
| `akkado/tests/test_pattern_generators.cpp` (new) | Unit tests for `run`, `binary`, `binaryN`. |
| `akkado/tests/test_voicing.cpp` (new) | Golden MIDI-note assertions for chord+anchor+mode combinations. |
| `akkado/tests/test_pattern_event.cpp` (new or existing) | Tests for `bend`, `aftertouch`, `dur` transforms and record-suffix parsing. |
| `web/static/docs/reference/pattern/transforms.md` (existing or new) | Document new transforms; update F1 help index via `bun run build:docs`. |
| `docs/mini-notation-reference.md` | Document `c4{vel:0.8, bend:0.2}` record suffix. |
| `docs/prd-pattern-array-note-extensions.md` | Status line + Phase 4/5/7/8 sections updated to defer to this PRD. |
| `docs/audits/prd-pattern-array-note-extensions_audit_2026-04-24.md` | Note that unmet goals are tracked in Phase 2. |

---

## 8. Implementation Phases

Staged so each sub-phase ships independently. Earlier phases unblock later ones (most notably, Phase A's `compile_pattern_for_transform()` extensions establish the patterns Phase B and Phase D will reuse).

### Phase A — Time & Structure Modifiers

**Goal:** All 12 transforms ship and chain via dot-call.

**Steps:**
1. Land the easy compile-time-only transforms first: `early`, `late`, `palindrome`, `compress`. (Pure event-list rewrites.) Drop the `compress` → `comp` alias.
2. Add `ply`, `linger`, `zoom`, `segment`. (Slightly more complex rewrites.)
3. Add `swing`, `swingBy`. (Slice-based timing offset.)
4. Add `SEQPAT_STEP` cycle-counter fields and implement `iter`, `iterBack`. (Touches Cedar.)

**Verification per step:**
- Unit test per transform: input pattern + expected output event list.
- Mini-notation/dot-call integration test for each: confirm `pat(...).x(args)` and `x(pat(...), args)` produce identical output.
- For `iter`/`iterBack`: Python experiment in `experiments/test_op_seqpat_step.py` exercising the cycle-counter rotation across multiple cycles.

**Acceptance:** `pat("c4 e4 g4 b4").palindrome().ply(2)` compiles, plays, and event list matches hand-computed expectation.

### Phase B — Algorithmic Generators

**Goal:** `run`, `binary`, `binaryN` ship as pattern constructors.

**Steps:**
1. Implement `run(n)` — simplest, validates the constructor pattern.
2. Implement `binary(n)`.
3. Implement `binaryN(n, bits)`.

**Verification:**
- Unit test per generator: assert event count, times, values match spec.
- Composition test: `run(8).fast(2).rev()` works (validates dot-call free composition).

**Acceptance:** `binary(0b10110010) |> sampler(%, "hh")` produces an audible 8-step rhythm matching the bit pattern.

### Phase C — Voicing System

**Goal:** `anchor`, `mode`, `voicing`, `addVoicings` ship with greedy voice leading.

**Steps:**
1. Create `voicing.hpp` / `voicing.cpp` with `voice_chords()` and the dictionary registry.
2. Implement built-in voicing dictionaries (`close`, `open`, `drop2`, `drop3`).
3. Wire `anchor`, `mode`, `voicing` into `compile_pattern_for_transform()` as voicing transforms on chord patterns.
4. Wire `addVoicings()` as a top-level statement that registers a dictionary at compile time.

**Verification:**
- Golden test fixtures: `(chord, anchor_midi, mode) → expected MIDI notes`.
- Voice-leading distance test: for a chord progression, total semitone movement is ≤ a hand-verified bound.
- Custom dictionary test: `addVoicings("test", {...})` then `chord(...).voicing("test")` round-trips.

**Acceptance:** `chord("Am C G F").anchor("c4").mode("below") |> mtof(%) |> poly(%, lead, 4) |> out(%, %)` plays with audible smooth voice leading.

### Phase D — Extended Note Properties

**Goal:** `bend`, `aftertouch`, `dur` transforms + record-suffix mini-notation.

**Steps:**
1. Extend `PatternEvent` with `bend`, `aftertouch`, `dur_override`, `properties`.
2. Implement `.bend()`, `.aftertouch()`, `.dur()` transforms (mirrors `.velocity()`).
3. Extend mini-notation lexer/parser for `c4{key:value,...}` suffix.
4. Wire record-suffix property values into `PatternEvent` (recognized → fixed field; unrecognized → `properties` bag).
5. Wire `properties` bag access through `as e |> ... e.cutoff` pipe-binding.

**Verification:**
- Transform tests: input event with default field, apply transform, assert field is set.
- Mini-notation test: parse `pat("c4{vel:0.8,bend:0.2}")`, assert resulting `PatternEvent.velocity == 0.8` and `event.bend == 0.2`.
- Custom property test: `pat("c4{cutoff:0.3}") as e |> lp(%, 200 + e.cutoff * 4000)` compiles to the expected cutoff.

**Acceptance:** `note("c4 e4 g4").bend("<0 0.5 -0.5>") |> mtof(% + bend(%) * 12) |> osc("sin", %)` audibly bends.

---

## 9. Edge Cases

### 9.1 Time Modifiers

- **`early(pat, n)` with `n > 1` or `n < 0`:** Wrap into `[0,1)` via modulo. `early(pat, 1.25)` ≡ `early(pat, 0.25)`.
- **`zoom(pat, 0, 0)` (degenerate range):** Error E13x: "zoom range must have positive width."
- **`zoom(pat, 0.5, 0.25)` (reversed range):** Error: "zoom end must exceed start."
- **`ply(pat, 0)` or `ply(pat, n)` with `n < 1`:** Error: "ply count must be ≥ 1."
- **`linger(pat, 0)` or `linger(pat, n)` with `n <= 0`:** Error.
- **`linger(pat, 1.0)`:** No-op (keep entire pattern).
- **`segment(pat, 1)`:** Single event spanning the cycle, valued at the event active at time 0.
- **`compress(pat, 0, 1)`:** No-op.
- **`compress(pat, 0, 0)`:** Empty pattern (zero duration).

### 9.2 Generators

- **`run(0)`:** Empty pattern.
- **`run(1)`:** Single event at time 0, value 0, duration 1.
- **`binary(0)`:** Single rest event (no bits set).
- **`binaryN(0, 0)`:** Empty pattern.
- **`binaryN(5, 2)`:** `n` exceeds `2^bits` — error or truncate to lower `bits` bits? **Decision: truncate to lower `bits` bits** (matches Strudel; defensible as bitmask semantics).
- **`binary(n)` with `n < 0`:** Error: "binary requires non-negative integer."

### 9.3 Voicing

- **`anchor()` without `mode()`:** Use a default mode of `"below"`.
- **`mode()` without `anchor()`:** Default anchor to `c4` (MIDI 60).
- **Empty chord progression:** Pass through unchanged.
- **Single chord:** No voice-leading happens; just apply mode/anchor.
- **Unknown voicing name:** Error E14x: "voicing dictionary 'X' not registered."
- **`addVoicings` called twice with same name:** Last write wins; no error (consistent with hot-reload workflow).

### 9.4 Extended Note Properties

- **`.bend(pat, val)` with `|val| > 1`:** Clamp to [-1, 1].
- **`.aftertouch(pat, val)` outside [0, 1]:** Clamp.
- **`.dur(pat, val)` with `val <= 0`:** Error.
- **Record suffix conflict with positional:** `c4:0.8{bend:0.2}` — both forms — combine: `:0.8` sets velocity, `{bend:0.2}` sets bend. No error.
- **Duplicate keys in record suffix:** `c4{vel:0.5, vel:0.7}` — last wins. No error (matches JS object semantics).
- **Reserved-name override:** `c4{velocity:0.5}` — the longer name `velocity` is also recognized as the fixed field.

### 9.5 Compatibility

- **`compress(signal, ratio, ...)`:** Will fail to type-check after the alias removal. Error message must point users to `comp(...)` / `compressor(...)`. Test that this error is emitted with a helpful message.
- **Existing tests using `compress` alias:** Must be updated to use `comp` or `compressor`. Audit `akkado/tests/` and the experiments directory.

---

## 10. Testing & Verification

### 10.1 Unit Tests

- **Per transform** (`akkado/tests/test_pattern_transforms.cpp`): build a pattern, apply transform, assert resulting `cedar::Event` list matches expected.
- **Per generator** (`akkado/tests/test_pattern_generators.cpp`): assert event times, values, durations.
- **Voicing** (`akkado/tests/test_voicing.cpp`): golden assertions per (chord, anchor, mode) tuple.
- **PatternEvent extensions**: assert `bend`/`aftertouch`/`dur_override` set correctly.
- **Mini-notation record suffix**: parse → assert AST contains expected `properties` vector.

### 10.2 Integration Tests

- `pat("c4 e4 g4 b4").early(0.25).palindrome()` — chained transforms produce expected event list.
- `run(8).fast(2)` — generator + transform composition.
- `chord("Am C G F").anchor("c4").mode("below")` — voicing produces expected MIDI notes.
- `pat("c4{vel:0.8,bend:0.2}") as e |> osc("sin", e.freq + e.bend * 100)` — full pipeline compiles and runs.

### 10.3 Experiments (Cedar runtime)

Per `docs/dsp-experiment-methodology.md` and the long-window guidance in `CLAUDE.md`:

- `experiments/test_op_seqpat_step.py` — extend with `iter` rotation tests across **at least 300 seconds** of simulated audio. Verify the pattern rotates correctly cycle-after-cycle, no voice drops, no timing drift.
- `experiments/test_op_palindrome.py` (new) — verify forward/backward alternation over 300+ s.

### 10.4 Manual Audition

- Chord progression with voicing leading: listen for smooth transitions, not octave jumps.
- Swing patterns at various amounts: confirm groove changes audibly.
- `bend` on a sustained note: confirm pitch bends.

### 10.5 Build & Lint

- `cmake --build build` clean.
- `bun run check` in `web/` clean.
- `bun run build:docs` regenerates the F1 lookup index for the new transforms.

---

## 11. Open Questions

- **Should `bend` be a per-event step value, or a continuous time-varying signal?** Phase 2 ships per-event (matches `velocity`). Continuous bend is a richer model that may want a separate signal-rate path; deferred.
- **Voicing dictionary sharing across compile units:** `addVoicings()` registers globally; should hot-reload preserve registrations? **Tentative:** yes — registry survives across recompiles, like `param()` values. Open until implementation.
- **`segment(pat, n)` on continuous (signal-rate) patterns:** the existing pattern eval is event-based; what does sampling a continuous source mean? **Tentative:** error for non-discrete patterns; revisit if/when continuous-pattern types land.

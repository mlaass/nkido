# PRD: Unison — Userspace Voice Multiplication with Detune, Width, and Phase Variation

> **Status: NOT STARTED** — Adds a `unison(...)` userspace stdlib function that multiplies a single instrument across N detuned, panned, phase-shifted voices, plus one supporting compiler change (`map(arr, fn)` accepts 2-arg `(val, idx)` closures).

## 1. Overview

Akkado today has two voice-related primitives:

- **`poly(input, voices, instrument)`** — a *runtime voice allocator*. Different incoming notes are assigned to different voices via oldest-voice stealing. Lowered to `POLY_BEGIN` / `POLY_END` opcodes with state-pool allocation.
- **`stereo(mono)`** / **`stereo(L, R)`** — pure signal duplication. No voice allocation, just `COPY` instructions.

What's missing is the third common primitive: **unison** — multiply a *single* note across N voices with detune, stereo width, and phase variation. Classic supersaw, fattened pads, doubled leads. Unlike `poly`, all unison voices play *the same note simultaneously*; they differ only in detune offset, stereo position, and phase.

This PRD ships `unison` as a **userspace stdlib function**, not a new opcode. Akkado already has the building blocks: first-class closures, `linspace`, `random`, `pan`, oscillators with `phase` parameters, and arithmetic on signals. The only compiler-side change is one ergonomic extension: `map(arr, fn)` should accept a 2-arg closure `(val, idx)` so userspace can index into parallel arrays (detune offsets, pans, phases) without a new `zipWith3` builtin.

If this composition turns out to be too expensive in practice (e.g. for `voices >= 8` with complex instruments), a follow-up PRD can promote it to a builtin opcode mirroring `poly`. Starting in userspace minimizes surface area and keeps the implementation transparent and editable.

### Why?

- **Live-coding ergonomics**: writing `unison(freq, gate, vel, voice, voices=5, detune=0.3)` is dramatically faster than hand-rolling five oscillators with offsets and pans.
- **Composability with `poly`**: a unisoned instrument is itself a 3-arg instrument that can be passed to `poly(...)`, giving polyphonic clusters for free without a special `poly + unison` combined opcode.
- **Foundation for more**: the `map`-with-index extension is broadly useful (sequencers, harmonics, indexed envelopes), not specific to unison.

### Major design decisions

- **Userspace, not a builtin opcode.** `unison` is a function in `STDLIB_SOURCE` (`akkado/include/akkado/stdlib.hpp`). All voice copies are statically expanded at compile time via `map`. Trade-off: voice count must be a compile-time constant, and the AST grows linearly with `voices`. This is acceptable for `voices ≤ 16`.
- **`map(arr, fn)` accepts 2-arg closures.** When the closure's `params.size() >= 2`, codegen dispatches to the existing `apply_binary_function_ref` and passes the element index as a constant buffer. Single-arg closures continue to work unchanged.
- **Instrument signature is `(freq, gate, vel, ext)`** where `ext` is a record `{idx, count, detune_st, pan, phase}`. Fully positional with `poly`'s `(freq, gate, vel)` for the first three params; the 4th `ext` is a record so future fields don't break existing code.
- **Monophonic semantics.** Unison plays one note at a time. Polyphonic unison is achieved by *wrapping* — define a unisoned instrument, then pass it to `poly`. No new `unison` × `poly` cross-product opcode is required.
- **Symmetric linear detune/width spread for v1.** `linspace(-detune, +detune, voices)` and `linspace(-width, +width, voices)`. Random and array-driven distributions are deferred to a future PRD.

---

## 2. Problem Statement

### What exists today

| Capability | Status | Reference |
|---|---|---|
| `poly(input, voices, instrument)` runtime allocator | ✅ Works | `akkado/src/codegen_functions.cpp:1437` |
| `stereo(mono)` / `stereo(L, R)` | ✅ Works | `akkado/src/codegen_stereo.cpp:152` |
| `pan(mono, position)` | ✅ Works | `builtins.hpp:637` (PAN opcode) |
| First-class closures `(p, …) -> body` | ✅ Works | `parser.cpp` (`parse_closure`) |
| Default closure params (`p = 0.5`) | ✅ Works | stdlib's `osc(type, freq, pwm = 0.5, …)` |
| `linspace(start, end, n)` | ✅ Works | `builtins.hpp:793` |
| `random(n)` deterministic per-call-site | ✅ Works | `codegen_arrays.cpp` (LCG seeded by semantic-ID) |
| Oscillator `phase` parameter | ✅ Works | every `OSC_*` builtin (`builtins.hpp:144`) |
| Record literals + field access | ✅ Works | parser + codegen |
| `map(arr, fn)` with 1-arg closure | ✅ Works | `codegen_arrays.cpp:292` |
| `map(arr, fn)` with 2-arg `(val, idx)` closure | ❌ Not supported | call site passes only one buffer |
| Stdlib as `.akk` file on disk | ❌ Embedded C++ string | `akkado/include/akkado/stdlib.hpp:10` |
| `unison(...)` builtin or stdlib function | ❌ Doesn't exist | — |

### What's missing

To write `unison` in user code today, you must construct N parallel voices that each take a *different* detune offset, *different* pan position, and *different* phase. The natural expression is `map(detunes, (d, idx) -> ...)` so the closure can read `pans[idx]` and `phases[idx]` from sibling arrays. But `map` only passes the value, not the index, and there is no `zipWith3` to bind three arrays into a 3-arg closure. Without one of these, the userspace expression devolves into hand-unrolled `voice0 + voice1 + voice2 + ...` for every voice count — defeating the purpose.

### Current vs proposed

| Today | After this PRD |
|---|---|
| Fattening a voice = hand-write N oscillators with offsets | `unison(freq, gate, vel, voice, voices=5, detune=0.3)` |
| `map(arr, (v) -> ...)` only | `map(arr, (v) -> ...)` *or* `map(arr, (v, idx) -> ...)` |
| Polyphonic unison requires a custom builtin | `poly(@, 4, fat_voice)` where `fat_voice` is a unisoned wrapper |
| Per-voice phase variation requires manually offsetting each oscillator | `instrument(freq, gate, vel, ext)` receives `ext.phase` per voice |

---

## 3. Goals and Non-Goals

### Goals

1. **Ship `unison` as userspace Akkado.** A single function in `STDLIB_SOURCE` produces a stereo signal with N detuned, panned, phase-shifted copies of a passed instrument. No new opcodes.
2. **Extend `map(arr, fn)`** to accept 2-arg closures `(val, idx) -> body`. The index is a const-valued buffer holding the integer position. Single-arg closures continue to work bit-identically.
3. **Compositional with `poly`.** A unisoned wrapper `(f, g, v) -> unison(f, g, v, voice, voices=N)` is a valid `poly` instrument. No special-case codegen.
4. **Predictable defaults.** `unison(freq, gate, vel, voice)` with no other args produces a sane fat sound (`voices=2`, `detune=0.5`, `width=0.5`, `phase=0`).
5. **Documentation + one demo patch.** Reference page in `web/static/docs/builtins/unison.md`, plus an example in `web/static/patches/unison-demo.akk` showing both standalone and `poly`-wrapped use.
6. **Zero regressions.** Existing patches that use `map`, `poly`, `stereo`, `pan`, oscillator `phase` — all continue to compile and produce identical bytecode.

### Non-Goals

- **No `UNISON_BEGIN` / `UNISON_END` opcode.** Voice expansion is static, not runtime. (May be revisited if perf demands.)
- **No random or array-driven detune distribution in v1.** Symmetric linear spread only. Future work.
- **No `voices` as a runtime/audio-rate value.** Voice count must be a compile-time integer literal (or constant expression). The AST is unrolled at codegen.
- **No automatic wrapping of 3-arg `poly` instruments.** Unison instruments must declare 4 params. A 1-line user wrapper bridges to `poly`-style instruments: `(f,g,v,_) -> existing(f,g,v)`. (Auto-arity dispatch could be a follow-up.)
- **No record mutation / per-block extras updates.** The `ext` record passed to the instrument is constructed once per voice at codegen time; its fields are constants for that voice across all blocks.
- **No combined `poly_unison` builtin.** Composition via wrapping is the supported path.
- **No detune LFO modulation built into unison.** Users add their own `lfo(...)` to the voice's freq inside the instrument if desired.

---

## 4. Target Syntax

### 4.1 Minimal usage

```akkado
// Define a 4-arg unison-compatible instrument
fn voice(freq, gate, vel, ext) -> 
    saw(freq) * ar(gate, 0.05, 0.4) * vel

// Drive it with a single note source
e = pat("c4") as evt
unison(evt.freq, evt.gate, evt.vel, voice) |> out(%, %)
```

Defaults: `voices=2, detune=0.5, width=0.5, phase=0`.

### 4.2 Full parameter set

```akkado
unison(
    freq, gate, vel, voice,
    voices = 5,        // 5 detuned copies
    detune = 0.3,      // ±0.3 semitones spread
    width = 0.8,       // ±0.8 stereo pan spread
    phase = 0.25       // up to 0.25 cycles of initial phase variation
) |> out(%, %)
```

### 4.3 Instrument that uses the `ext` record

```akkado
fn rich_voice(freq, gate, vel, ext) -> 
    let detuned = saw(freq, ext.phase)               // initial phase from unison
    let env = ar(gate, 0.05 + ext.idx * 0.005, 0.4)  // tiny per-voice attack stagger
    detuned * env * vel
```

The `ext` record contains:
- `ext.idx` — voice index, 0..voices-1
- `ext.count` — total voice count (= `voices` arg)
- `ext.detune_st` — this voice's detune offset in semitones
- `ext.pan` — this voice's pan position, -1..+1
- `ext.phase` — this voice's initial phase offset, 0..1

### 4.4 Bridging a 3-arg `poly` instrument

```akkado
fn lead(freq, gate, vel) -> sqr(freq) * adsr(gate, 0.01, 0.1, 0.7, 0.3)

// Wrap to ignore the ext record
fn fat_lead(f, g, v, _) -> lead(f, g, v)

unison(440, 1, 1, fat_lead, voices = 5, detune = 0.4) |> out(%, %)
```

### 4.5 Composition with `poly` (polyphonic clusters)

```akkado
// fat_voice itself is a 3-arg instrument compatible with poly()
fn fat_voice(freq, gate, vel) -> 
    unison(freq, gate, vel, (f, g, v, ext) -> saw(f) * v, voices = 4, detune = 0.2)

pat("c4 e4 g4 b4") |> poly(@, 4, fat_voice) |> out(%, %)
```

Each note triggers a 4-voice unison cluster; up to 4 notes ring simultaneously → up to 16 oscillators. CPU scales with `poly_voices × unison_voices`.

### 4.6 `map` with index (the supporting change)

```akkado
// Generate harmonics with per-element phase staggering
freqs = [220, 440, 660, 880]
phases = [0, 0.25, 0.5, 0.75]
voices = map(freqs, (f, idx) -> sine(f, phases[idx]))
sum(voices) |> out(%, %)
```

Existing 1-arg form is unchanged:

```akkado
sigs = map([220, 440], (f) -> sine(f))   // still works
```

---

## 5. Architecture

### 5.1 Userspace `unison` definition

Added to `STDLIB_SOURCE` in `akkado/include/akkado/stdlib.hpp`:

```akkado
fn unison(freq, gate, vel, instrument,
          voices = 2, detune = 0.5, width = 0.5, phase = 0) -> {
    let detunes = linspace(-detune, detune, voices)
    let pans    = linspace(-width,  width,  voices)
    map(detunes, (d, idx) -> {
        let v_freq  = freq * pow(2, d / 12)
        let v_pan   = pans[idx]
        let v_phase = (idx / voices) * phase
        let ext = {
            idx:        idx,
            count:      voices,
            detune_st:  d,
            pan:        v_pan,
            phase:      v_phase
        }
        pan(instrument(v_freq, gate, vel, ext), v_pan)
    }) |> sum_stereo(%)
}
```

**`sum_stereo`** is a small companion stdlib helper that sums an array-of-stereo-signals into one stereo signal by summing left and right buffers separately. Implemented with `reduce`:

```akkado
fn sum_stereo(arr) -> 
    reduce(arr, (acc, s) -> stereo(left(acc) + left(s), right(acc) + right(s)),
           stereo(0, 0))
```

(If `sum` already correctly handles arrays of stereo signals after the `pan()` returns multi-buffer values, `sum_stereo` collapses to `sum`. See §10 Q1.)

### 5.2 Compiler change: `map` with index

**File:** `akkado/src/codegen_arrays.cpp` (`handle_map_call`, line 292).

Today (line 322): `apply_function_ref(*func_ref, element_buffers[i], n.location)`.

After:

```cpp
if (func_ref->params.size() >= 2) {
    // Allocate a constant buffer holding the integer index (as float)
    std::uint16_t idx_buf = emit_const(static_cast<float>(i), buffers_, instructions_);
    result_buffers.push_back(
        apply_binary_function_ref(*func_ref, element_buffers[i], idx_buf, n.location));
} else {
    result_buffers.push_back(apply_function_ref(*func_ref, element_buffers[i], n.location));
}
```

Same dispatch in the single-buffer branch (line 310). `apply_binary_function_ref` already exists at `codegen_arrays.cpp:213` and is the same primitive `reduce` and `zipWith` use.

The index buffer is a single `CONST` instruction emitting `float(i)` into a fresh buffer — no new opcode needed.

### 5.3 Voice expansion (compile-time)

```
unison(freq, gate, vel, voice, voices=4, detune=0.3, width=0.6, phase=0.1)
                            │
                            ▼  (stdlib expansion)
let detunes = linspace(-0.3, 0.3, 4)   // [-0.3, -0.1, 0.1, 0.3]
let pans    = linspace(-0.6, 0.6, 4)
                            │
                            ▼  (map with idx)
map([-0.3, -0.1, 0.1, 0.3], (d, idx) -> ...)
                            │
                            ▼  (codegen unrolls map over multi-buffer)
voice0_buf = pan(voice(freq * pow(2,-0.3/12), gate, vel, ext0), -0.6)
voice1_buf = pan(voice(freq * pow(2,-0.1/12), gate, vel, ext1), -0.2)
voice2_buf = pan(voice(freq * pow(2, 0.1/12), gate, vel, ext2),  0.2)
voice3_buf = pan(voice(freq * pow(2, 0.3/12), gate, vel, ext3),  0.6)
                            │
                            ▼  (sum_stereo)
stereo(L0+L1+L2+L3, R0+R1+R2+R3)
```

The semantic-ID path mechanism gives each voice's stateful sub-nodes (oscillators, envelopes) stable distinct hashes via the `map#N/elemK/...` path scheme already used by `map`, so hot-swap state preservation works automatically.

### 5.4 Constraints

| Limit | Value | Rationale |
|---|---|---|
| `voices` range | 1..16 | Compile-time AST unrolling. >16 risks compile time blow-up and arena pressure. |
| `voices` form | Compile-time integer literal | Required by `linspace(_, _, voices)` and `map` unrolling. |
| `detune` form | Audio-rate float (signal or const) | `linspace` produces a static array of constants; `detune` is read at codegen. |
| `width` form | Same as `detune` | Same. |
| `phase` form | Same as `detune` | Same. |
| Instrument arity | Exactly 4 | `(freq, gate, vel, ext)`. 3-arg `poly` instruments require a wrapper. |
| Output | Stereo signal | Always; users wanting mono can do `0.5 * (left(u) + right(u))`. |

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `POLY_BEGIN` / `POLY_END` opcodes | **Stays** | No changes. |
| `STEREO_*` / `PAN` opcodes | **Stays** | No changes. |
| `OSC_*` opcodes | **Stays** | Phase parameter already exists and is used directly. |
| `map(arr, 1-arg-fn)` semantics | **Stays** | Preserved bit-for-bit; new code path only triggers when `params.size() >= 2`. |
| `map(arr, 2-arg-fn)` | **New** | Closure receives `(val, idx)`. |
| `apply_binary_function_ref` | **Stays** | Already exists; reused, not modified. |
| `STDLIB_SOURCE` | **Modified** | Adds `fn unison(...)` and `fn sum_stereo(...)` (if needed). |
| `STDLIB_LINE_COUNT` | **Auto-adjusts** | constexpr lambda; recomputes. |
| `poly()` codegen | **Stays** | Composability is via wrapping, not codegen integration. |
| Hot-swap state preservation | **Stays** | Existing semantic-ID path scheme covers `map#N/elemK/...` already. |
| Web docs index | **Modified** | New entry for `unison` builtin. |
| F1 help lookup | **Modified** | Built from frontmatter keywords; rebuild via `bun run build:docs`. |

---

## 7. File-Level Changes

### Modify

| File | Change |
|---|---|
| `akkado/include/akkado/stdlib.hpp` | Add `fn unison(...)` and (if needed) `fn sum_stereo(...)` to `STDLIB_SOURCE`. |
| `akkado/src/codegen_arrays.cpp` | In `handle_map_call` (line 292) — dispatch to `apply_binary_function_ref` when `params.size() >= 2`, allocating a const index buffer per element. |
| `akkado/include/akkado/builtins.hpp` | Update `map` description (line 701) to mention 2-arg `(val, idx)` form. |
| `akkado/tests/test_codegen.cpp` | New test cases for `map`-with-index and `unison` (see §11). |
| `web/static/docs/builtins/map.md` | Document 2-arg closure form with example. |
| `web/static/docs/builtins/index.md` (or per-section index) | Add `unison` entry. |

### Create

| File | Purpose |
|---|---|
| `web/static/docs/builtins/unison.md` | Reference page: signature, params, instrument convention, examples (basic, with `ext`, composed with `poly`). |
| `web/static/patches/unison-demo.akk` | Demo patch showing standalone unison and a `poly(@, 4, unisoned)` cluster. |
| `experiments/test_unison.py` | Python opcode-level smoke test: render unison output for a simple `saw` voice, verify N spectral peaks around target freq, save WAV. |

### Stays

| File | Reason |
|---|---|
| `cedar/include/cedar/opcodes/*` | No new opcodes. |
| `cedar/include/cedar/vm/instruction.hpp` | No new opcodes. |
| `akkado/src/codegen_functions.cpp` `handle_poly_call` | Unison composes with poly via wrapping; poly itself is unchanged. |
| `akkado/src/codegen_stereo.cpp` | `pan()` and `stereo()` are reused unchanged. |

---

## 8. Implementation Phases

### Phase 1 — `map` with index

**Status**: TODO  
**Goal**: `map(arr, (val, idx) -> body)` works for any array (single-buffer or multi-buffer). Single-arg form unchanged.

Files to modify:
- `akkado/src/codegen_arrays.cpp` — extend `handle_map_call` (both single- and multi-buffer branches).
- `akkado/include/akkado/builtins.hpp` — refresh `map` description.
- `akkado/tests/test_codegen.cpp` — add tests under `[map][index]`.

Tasks:
- [ ] Detect 2-arg closure via `func_ref->params.size() >= 2` in `handle_map_call`.
- [ ] Allocate const index buffer per element (use existing `emit_const` helper).
- [ ] Dispatch to `apply_binary_function_ref` when 2-arg.
- [ ] Verify single-arg path produces bytecode identical to current `master` (snapshot test).
- [ ] Test: `map([10, 20, 30], (v, i) -> v + i)` returns `[10, 21, 32]`.
- [ ] Test: existing 1-arg map calls in fixtures unchanged.
- [ ] Test: closure with 3+ params triggers existing param-count error (E140 or new error).

### Phase 2 — `unison` stdlib function

**Status**: TODO  
**Goal**: `unison(freq, gate, vel, instrument, voices=2, detune=0.5, width=0.5, phase=0)` compiles and produces a stereo signal matching expected spectral content.

Depends on: Phase 1.

Files to modify:
- `akkado/include/akkado/stdlib.hpp` — add `fn unison`, possibly `fn sum_stereo`.
- `akkado/tests/test_codegen.cpp` — `[unison]` tag with compile + bytecode snapshot tests.

Tasks:
- [ ] First investigate Q1 (§10): does `sum(arr_of_stereo_signals)` already do the right thing? If yes, skip `sum_stereo`.
- [ ] Add `unison` to `STDLIB_SOURCE`.
- [ ] Test: `unison(440, 1, 1, (f,g,v,e) -> sine(f))` compiles with all defaults.
- [ ] Test: `unison(..., voices = 5, detune = 0.3)` compiles; bytecode contains 5 sine instances at distinct semantic-IDs.
- [ ] Test: `unison(..., voices = 1)` produces a single voice (no panning, no detune — degenerate but valid).
- [ ] Test: error on `voices = 0` (should emit error, not silent or crash).
- [ ] Test: error on `voices > 16`.
- [ ] Test: `voices` not a compile-time literal → error.
- [ ] Test: composing with `poly` — `poly(@, 4, fat_voice)` where `fat_voice` calls `unison` internally — compiles and produces 16 oscillator instances total.

### Phase 3 — Documentation and demo

**Status**: TODO  
**Goal**: Web docs explain unison; demo patch sounds great.

Tasks:
- [ ] Write `web/static/docs/builtins/unison.md` with frontmatter keywords (`unison, supersaw, fatten, detune, voices`).
- [ ] Update `web/static/docs/builtins/map.md` with 2-arg form.
- [ ] Run `bun run build:docs` to refresh the F1 lookup index.
- [ ] Write `web/static/patches/unison-demo.akk` — at least two presets (a fat lead, a polyphonic pad).
- [ ] Add a brief mention in `CLAUDE.md` Akkado concepts section if appropriate.

### Phase 4 — Python opcode smoke test

**Status**: TODO  
**Goal**: A Cedar-level test in `experiments/` that renders unison output and saves a WAV for ear evaluation.

Tasks:
- [ ] Compile a small unison patch via `akkado-cli` to bytecode.
- [ ] Render N seconds via `cedar_core` Python bindings.
- [ ] Verify spectrum has approximately `voices` peaks within ±detune semitones of the fundamental.
- [ ] Save WAV to `output/op_unison/`.

### Phase 5 (FUTURE — out of scope here)

If perf becomes a problem at large `voices` counts, promote `unison` to a builtin opcode (`UNISON_BEGIN` / `UNISON_END`) mirroring the `poly()` lowering path. That would be a separate PRD with full opcode design, voice-pool allocation, and runtime detune/width parameters.

Other future work, also separate PRDs:
- Random and array-driven detune/pan/phase distributions.
- Audio-rate `voices` count (not feasible with userspace expansion).
- `unison` × `poly` combined opcode if composition overhead matters.
- Auto-arity dispatch so 3-arg poly instruments work in unison without a manual wrapper.

---

## 9. Edge Cases

| Situation | Expected behavior | Rationale |
|---|---|---|
| `voices = 1` | Single voice, no detune (linspace(-d, d, 1) = [0]), pan = 0, phase = 0. Wraps a single `instrument(...)` call with `pan(_, 0)`. | Degenerate but legal; useful for A/B testing the unison wrapper. |
| `voices = 0` | Compile error: `E???: unison voices must be >= 1`. | Otherwise produces silence with no instrument calls — confusing. |
| `voices = 17` (or >16) | Compile error: `E???: unison voices must be <= 16`. | Static AST unrolling; arena/buffer pressure. |
| `voices` not a literal | Compile error from `linspace` (already enforces this). | linspace already needs compile-time `n`. |
| `detune = 0`, `width = 0`, `phase = 0` | All voices identical, all centered, all in-phase → equivalent to `voices * instrument(...)` (a louder single voice). | Mathematically correct; user error to set all three to 0 but harmless. |
| `instrument` has 3 params instead of 4 | Existing E404 (wrong param count) at the call site inside `map`. | Documented; require manual wrapper. |
| `instrument` has 5+ params | Same E404. | Same. |
| `instrument` returns stereo (multi-buffer) signal | `pan(stereo_sig, p)` semantics — see Q3 §10. | Open question. |
| Negative `detune` or `width` | Treated as positive (linspace symmetry); no error. | linspace(-(-0.3), -0.3, 5) = linspace(0.3, -0.3, 5) — valid. |
| `phase > 1` | Each voice gets `(idx/voices) * phase`; oscillators wrap modulo 1 internally. | Existing oscillator behavior; no special handling needed. |
| Hot-swap with `voices` change | Existing patch's voices get fresh state (different `map#N/elemK` paths). | Acceptable — voice count change is structural; micro-crossfade applies. |
| Hot-swap with same `voices`, different `detune` | Existing voice states preserved; only frequency-multiplier constants change. | Standard hot-swap behavior. |
| `freq = 0` | Each oscillator gets a (possibly slightly detuned-from-zero) freq; `pow(2, d/12) * 0 = 0`. Output: silence. | Correct. |

---

## 10. Open Questions

- **Q1 — Does `sum(arr_of_stereo_signals)` work today, or is `sum_stereo` needed?**
  Need to test: when `pan(...)` returns a multi-buffer (stereo) signal and `map` produces an array of those, does the existing `sum` correctly fold L into L and R into R, or does it flatten? Inspect `handle_sum_call` (`codegen_arrays.cpp:331`) and write a small test. If `sum` works, drop `sum_stereo` from the design.

- **Q2 — Detune unit: semitones, cents, or normalized?**
  v1 proposal is **semitones** (`detune = 0.3` → ±0.3 semitones = ±30 cents, classic supersaw range). Alternatives: cents (`detune = 30` for ±30 cents), or 0..1 normalized (`detune = 0.3` = 30% of an octave). Recommend semitones for proximity to musical thinking. Confirm before Phase 2.

- **Q3 — Stereo instruments inside unison?**
  If a user passes `(f,g,v,e) -> stereo(saw(f), saw(f*1.01))`, `unison`'s `pan(stereo_sig, p)` may not behave correctly. Does `pan` accept stereo input? If not, document that unison instruments must return mono.

- **Q4 — What does `pow` resolve to in stdlib?**
  `freq * pow(2, d / 12)` requires a `pow` builtin or operator. Akkado may have `^` instead. Check: if no `pow` exists, use `^` operator or add a `pow(base, exp)` builtin (or use stdlib helper).

- **Q5 — Should `phase` arg name conflict with oscillator `phase` param confuse users?**
  `unison(..., phase = 0.25)` controls the *spread* of phase across voices, not an absolute phase. Documentation should be clear; possibly rename to `phase_spread` or `phase_jitter`. Recommend keeping `phase` for brevity but documenting prominently.

- **Q6 — Equal-power vs linear panning inside unison?**
  Does `pan()` use equal-power law (cos/sin) or linear? For unison, equal-power preserves perceived volume regardless of `width`. Verify and document; if `pan` is linear, unison may need its own panning math.

- **Q7 — Should `map` with index error or warn if a 1-arg closure is passed where a 2-arg was expected?**
  No — `map` doesn't expect any specific arity. The proposal is pure dispatch on what the closure declares. No warning needed.

---

## 11. Testing / Verification Strategy

### Phase 1 — `map` with index

**Akkado tests** (`akkado/tests/test_codegen.cpp`, tag `[map][index]`):

```cpp
SECTION("map with 2-arg closure receives index") {
    // arr = [10, 20, 30]
    // result = map(arr, (v, i) -> v + i) → [10, 21, 32]
    auto result = compile_and_extract_array("map([10,20,30], (v, i) -> v + i)");
    REQUIRE(result == std::vector<float>{10.0f, 21.0f, 32.0f});
}

SECTION("map with 1-arg closure unchanged") {
    auto bc_before = compile_to_bytecode_str_master("map([1,2,3], (v) -> v * 2)");
    auto bc_after  = compile_to_bytecode_str_branch("map([1,2,3], (v) -> v * 2)");
    REQUIRE(bc_before == bc_after);
}

SECTION("map with 3-arg closure errors") {
    REQUIRE_THROWS_WITH(compile("map([1], (a,b,c) -> a)"), Catch::Contains("E140"));
}
```

### Phase 2 — `unison`

**Akkado tests** (`akkado/tests/test_codegen.cpp`, tag `[unison]`):

```cpp
SECTION("unison compiles with default args") {
    auto src = R"(
        fn voice(f, g, v, e) -> sine(f)
        unison(440, 1, 1, voice) |> out(%, %)
    )";
    REQUIRE_NOTHROW(compile(src));
}

SECTION("unison emits N voice instances") {
    // voices=5 → 5 SINE opcodes, each with distinct state_id
    auto bc = compile_to_bytecode("unison(440, 1, 1, (f,g,v,e) -> sine(f), voices=5)");
    auto sine_count = count_opcode(bc, Opcode::SINE);
    REQUIRE(sine_count == 5);
    auto state_ids = collect_state_ids(bc, Opcode::SINE);
    REQUIRE(unique(state_ids).size() == 5);
}

SECTION("unison voices=1 is degenerate but valid") {
    REQUIRE_NOTHROW(compile("unison(440, 1, 1, (f,g,v,e) -> sine(f), voices=1)"));
}

SECTION("unison voices=0 errors") {
    REQUIRE_THROWS_WITH(compile("unison(440, 1, 1, (f,g,v,e) -> sine(f), voices=0)"),
                        Catch::Contains("voices"));
}

SECTION("unison composes with poly") {
    auto src = R"(
        fn voice(f, g, v, e) -> saw(f)
        fn fat(f, g, v) -> unison(f, g, v, voice, voices=4)
        pat("c4 e4") |> poly(@, 2, fat) |> out(%, %)
    )";
    REQUIRE_NOTHROW(compile(src));
    // Expect 8 SAW opcodes total: 2 poly voices × 4 unison voices.
    auto bc = compile_to_bytecode(src);
    REQUIRE(count_opcode(bc, Opcode::SAW) == 8);
}
```

### Phase 4 — Python smoke test

**File**: `experiments/test_unison.py`

```python
def test_unison_spectrum():
    """
    unison(440, 1, 1, voice, voices=5, detune=0.3) should produce 5 spectral peaks
    spread approximately ±0.3 semitones around 440Hz (~±7.6Hz).
    """
    src = '''
        fn voice(f, g, v, e) -> saw(f)
        unison(440, 1, 1, voice, voices=5, detune=0.3) |> out(%, %)
    '''
    audio = render(src, seconds=1.0)
    peaks = find_spectral_peaks(audio, min_db=-20)
    # Should see 5 distinct peaks within ±10 Hz of 440
    nearby_peaks = [f for f in peaks if abs(f - 440) < 20]
    assert len(nearby_peaks) == 5, f"Expected 5 peaks, found {len(nearby_peaks)}"

    wav_path = os.path.join(OUT, "unison_5voice.wav")
    scipy.io.wavfile.write(wav_path, SR, audio)
    print(f"  Saved {wav_path} - Listen for fat detuned saw, voices spread across stereo")
```

### Acceptance criteria

- [ ] All existing tests pass without modification.
- [ ] `map` with 1-arg closure produces byte-identical bytecode to pre-change `master`.
- [ ] `map` with 2-arg closure correctly receives integer indices.
- [ ] `unison` compiles for `voices` in {1, 2, 4, 8, 16}.
- [ ] `unison` errors clearly for `voices` in {0, 17, non-literal}.
- [ ] `unison` composed with `poly` produces `poly_voices × unison_voices` instrument instances.
- [ ] Demo patch `unison-demo.akk` plays without glitches at default audio settings (48kHz / 128-block).
- [ ] Hot-swap test: edit a unison patch's `detune` from 0.3 to 0.5 in the web UI; voices retain envelope state across the swap.
- [ ] WAV output from the Python test sounds like a fat detuned chord (human ear evaluation).

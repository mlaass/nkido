# Review: Patterns as First-Class Data in Akkado

> A comprehensive analysis of the gap between Akkado's pattern system and its typed data primitives, with approaches to bridge them.

## Context

Akkado has a powerful mini-notation system for defining musical patterns (`pat("c4 e4 g4")`), and a rich set of typed primitives (records, arrays, numbers, signals). However, these two worlds are largely disconnected. Patterns are opaque compile-time artifacts — you can create them from strings, access their output fields, and apply a fixed set of transforms, but you cannot construct, decompose, inspect, or manipulate them using the same record/array/number primitives that the rest of the language provides.

This document catalogs what's already possible, identifies the gaps, and discusses approaches to close them.

---

## 1. What Already Works

### Pattern Creation
- **Mini-notation strings**: `pat("c4 e4 g4")`, `pat("bd sd [hh hh] cp")`
- **Chord patterns**: `chord("Am C7 F G")` with Strudel-compatible symbol parsing
- **Algorithmic**: `euclid(k, n, rot)` as a Cedar opcode (runtime), Euclidean syntax `bd(3,8)` in mini-notation
- **Timeline curves**: `timeline("0 0.5 1 0.5")` for automation breakpoints

### Pattern Field Access (5 fixed fields)
- `%.freq` / `%.pitch` / `%.f` — frequency (Hz)
- `%.vel` / `%.velocity` / `%.v` — velocity (0–1)
- `%.trig` / `%.trigger` / `%.t` — trigger pulse
- `%.gate` / `%.g` — gate signal
- `%.type` — event type ID (for sample routing)

These are extracted into audio-rate buffers by `SEQPAT_STEP`/`SEQPAT_GATE`/`SEQPAT_TYPE` opcodes at block boundaries.

### Named Bindings and Destructuring
```akkado
pat("c4 e4 g4") as e |> osc("sin", e.freq) |> % * e.vel
pat("c4 e4 g4") as {freq, vel} |> osc("sin", freq) |> % * vel
```

### Compile-Time Pattern Transforms
`slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `variant` — all modify the compiled `Sequence` event list before bytecode emission. Callable via both functional and dot-call syntax:
```akkado
pat("c4 e4 g4").slow(2).transpose(12).velocity(0.8)
```

### Type System Foundation
`TypedValue` tracks 8 types: Signal, Number, Pattern, Record, Array, String, Function, Void. The compiler knows when something is a Pattern and can type-check field access, transforms, and polyphonic dispatch.

### Records and Arrays (the "other world")
```akkado
// Records — fully typed, spread, field access
rec = {freq: 440, vel: 0.8}
modified = {..rec, freq: 880}

// Arrays — compile-time unrolled, functional ops
[1, 2, 3] |> map(%, x -> x * 2)
sum([1, 2, 3])
zip(freqs, vels)
```

---

## 2. The Gaps

### Gap 1: No programmatic pattern construction

You cannot build a pattern from arrays of records:
```akkado
// IMPOSSIBLE today:
events = [{freq: 440, time: 0.0, dur: 0.5}, {freq: 880, time: 0.5, dur: 0.5}]
my_pat = pattern(events)  // ← does not exist
```

Patterns can *only* originate from string literals parsed by the mini-notation parser, or from the `euclid` opcode. There's no way to use the language's own data structures (records, arrays, numbers) to define what a pattern contains.

### Gap 2: No decomposition of patterns into language values

You cannot get a pattern's events out as an array:
```akkado
// IMPOSSIBLE today:
p = pat("c4 e4 g4")
events = p.events      // ← does not exist
first = events[0]      // ← can't index into pattern events
first.freq             // ← can't read event fields as compile-time values
len(events)            // ← can't count events
```

The only way to "read" a pattern is through its 5 audio-rate field buffers (`freq`, `vel`, `trig`, `gate`, `type`), which produce one value per block at runtime. The individual events — their times, durations, velocities, note values — are invisible to the language.

### Gap 3: No event-level manipulation

You cannot map, filter, or transform individual events using the language's functional tools:
```akkado
// IMPOSSIBLE today:
pat("c4 e4 g4 b4")
  |> filter_events(%, e -> e.freq > 400)    // ← keep only high notes
  |> map_events(%, e -> {..e, vel: e.vel * 0.5})  // ← halve velocities
```

The existing transforms (`slow`, `transpose`, etc.) are a closed set of compiler-recognized functions. Users cannot define their own event-level transformations.

### Gap 4: No pattern composition from parts

You cannot merge, concatenate, or interleave patterns:
```akkado
// IMPOSSIBLE today:
kicks = pat("bd ~ bd ~")
snares = pat("~ sd ~ sd")
combined = stack(kicks, snares)   // ← does not exist
appended = cat(kicks, snares)     // ← does not exist
```

The only way to layer patterns is via the mini-notation itself (`[bd, sd]` for polyrhythm) or by running separate pattern pipelines to the same `out()`.

### Gap 5: Limited event fields

`PatternPayload` exposes exactly 5 fields: `freq`, `vel`, `trig`, `gate`, `type`. The underlying `Event` struct also has `time`, `duration`, `chance`, `source_offset` — but these are not accessible from the language. User-defined fields (e.g., `pan`, `cutoff`, `detune`) are not possible.

### Gap 6: No runtime pattern construction or modification

All pattern data is baked into `StateInitData` at compile time. There's no mechanism to:
- Build patterns from runtime values (e.g., random note sequences)
- Modify a pattern's events based on a live control signal
- Conditionally include/exclude events at runtime

This is partially by design (zero-allocation constraint), but the boundary between "compile-time programmable" and "runtime opaque" is more restrictive than it needs to be.

---

## 3. What Existing Work Covers (and Doesn't)

### PRD-Pattern-Array-Note-Extensions
- **Covers**: Array type, map(), chord system, dot-call, polymeter, time/structure modifiers
- **Doesn't cover**: Programmatic event construction, pattern decomposition, event-level map/filter, user-defined event fields

### Vision-Language-Evolution
- **Covers**: `state` keyword, delay lines, module system, type system, const fn
- **Doesn't cover**: Pattern ↔ record/array bridging. The vision document classifies pattern transforms as "needs Pattern as first-class type" but defines "first-class" as "method chaining + type checking", not "constructible/decomposable from language primitives"

### PRD-Compiler-Type-System
- **Covers**: TypedValue with Pattern variant, field access, type checking
- **Doesn't cover**: Pattern internals as structured data accessible to the language

### PRD Records and Field Access
- **Covers**: Record literals, field access, pipe binding, pattern event fields via `%`
- **Doesn't cover**: Events-as-records, constructing patterns from records

---

## 4. Approaches to Close the Gaps

### Approach A: Compile-Time Event Arrays (Recommended Starting Point)

**Core idea**: Let patterns be constructed from and decomposed into arrays of event records at compile time.

```akkado
// Construction: array of event records → pattern
my_pat = pattern([
  {note: 60, time: 0.0, dur: 0.5, vel: 1.0},
  {note: 64, time: 0.5, dur: 0.5, vel: 0.8},
  {note: 67, time: 1.0, dur: 0.5, vel: 0.6}
])

// Decomposition: pattern → array of event records
events = pat("c4 e4 g4").events

// Manipulation: use existing array tools
loud = events |> map(%, e -> {..e, vel: 1.0})
high_only = events |> filter(%, e -> e.note > 62)
with_pan = events |> map(%, (e, i) -> {..e, pan: i / len(events)})

// Recompose
pattern(loud) |> osc("sin", %.freq) |> out(%, %)
```

**Why this fits Akkado**:
- All operations are compile-time — no runtime allocations, no new opcodes
- Leverages existing record spread, array map/fold/filter, and pattern codegen
- The `SequenceCompiler` already produces `Event` lists — this just exposes that representation upward to the language level
- Consistent with Akkado's Faust-style "compile-time composition" philosophy

**Implementation sketch**:
1. Define a canonical event record shape: `{note, time, dur, vel, chance, type_id, ...}`
2. Add `pattern(events_array)` constructor that takes an Array of Records and feeds them into `SequenceCompiler` (or directly into `Sequence` structs)
3. Add `.events` accessor on Pattern values that returns the compiled event list as an Array of Records
4. Events round-trip: `p.events |> pattern(%)` should be identity

**What this unlocks**:
- Programmatic pattern generation via `range()` + `map()`
- Event filtering, sorting, shuffling using existing array ops
- User-defined transforms as regular functions
- Pattern merging via array concatenation
- Algorithmic composition (e.g., generate events from mathematical sequences)

**Complexity**: Medium. No new VM opcodes. Requires bridging between `RecordPayload`/`ArrayPayload` and `SequenceCompiler` event data. The compile-time constraint means all array operations must resolve to constants.

### Approach B: User-Defined Event Fields

**Core idea**: Extend the 5-field `PatternPayload` with user-defined fields.

```akkado
// Attach arbitrary fields in mini-notation via method syntax
pat("c4 e4 g4").set("pan", [0.0, 0.5, 1.0])

// Or via event records
pattern([
  {note: 60, pan: 0.0, cutoff: 2000},
  {note: 64, pan: 0.5, cutoff: 4000},
]) as e |> osc("sin", e.freq) |> pan(%, e.pan) |> lp(%, e.cutoff)
```

**Implementation considerations**:
- Each custom field needs a buffer allocation (like the existing freq/vel/trig/gate/type)
- The `PatternPayload::fields` array is currently fixed at 5 — could switch to a map or extend the array with a dynamic section
- Field names would need compile-time resolution (string interning already exists)
- `SEQPAT_STEP` would need to support additional output buffers

**Complexity**: Medium-High. Touches the Cedar VM's `SEQPAT_*` opcodes and `SequenceState`. But the mechanism is a natural extension of what already exists.

### Approach C: Pattern Combinators

**Core idea**: Provide composition operators that work on Pattern-typed values.

```akkado
// Stack (simultaneous)
stack(pat("bd ~ bd ~"), pat("~ sd ~ sd"))

// Concatenate (sequential)
cat(pat("c4 e4"), pat("g4 b4"))

// Interleave (alternating events)
interleave(pat("c4 e4 g4"), pat("d4 f4 a4"))

// Conditional
when(beat(4), pat("cp"), pat("~"))
```

**This is partially achievable today** if patterns are decomposable to event arrays (Approach A), since `stack` = array concat + adjust times, `cat` = array concat with offset, etc. But dedicated combinators would be more ergonomic and could optimize the compiled output.

**Complexity**: Low-Medium if built on top of Approach A. Each combinator is a compile-time function that manipulates event arrays.

### Approach D: Runtime Pattern Mutation (Deferred)

Full runtime pattern manipulation (e.g., events chosen by live signals) would require:
- Runtime event arrays in the Cedar VM
- Dynamic `Sequence` rebuilding within the audio thread (violates zero-alloc)
- Or: a pre-allocated event pool with runtime selection masks

This conflicts fundamentally with Cedar's zero-allocation constraint. The pragmatic path is to keep pattern *structure* compile-time but allow runtime *parameter modulation* of event fields (which already works via the buffer system — `%.freq` is a runtime signal).

**Recommendation**: Defer. The compile-time approach (A+B+C) covers the vast majority of use cases. Runtime mutation can be revisited if/when the `state` keyword lands, which would enable user-defined sequencers.

---

## 5. Recommended Phasing

### Phase 1: Event Record Bridge (Approach A)
- `pattern(array_of_records)` — construct pattern from event records
- `.events` accessor — decompose pattern to event record array
- Canonical event record shape with well-defined fields
- Round-trip guarantee: `p.events |> pattern(%)` ≡ `p`
- **This is the keystone**: once events are arrays of records, all existing array/record tools apply automatically

### Phase 2: Pattern Combinators (Approach C)
- `stack(p1, p2, ...)` — layer patterns
- `cat(p1, p2, ...)` — sequence patterns
- `interleave(p1, p2)` — alternate events
- Built on Phase 1 internals (event array manipulation)

### Phase 3: User-Defined Event Fields (Approach B)
- Extend `PatternPayload` beyond the 5 fixed fields
- Support arbitrary named fields in event records
- Dynamic `SEQPAT_STEP` buffer allocation for custom fields

### Phase 4: Algorithmic Constructors
- `pattern_from(n, fn)` — generate n events from a function
- `subdivide(n)` — n equal-time events with index
- Integration with `range()`, `linspace()`, etc.

---

## 6. Key Design Questions

1. **Event record shape**: What fields are canonical? Minimal (`{note, time, dur, vel}`) vs. maximal (include `chance`, `type_id`, `sample`, `bank`, etc.)? Should unknown fields pass through silently or error?

2. **Compile-time only or const-fn evaluable?**: If `pattern()` only accepts literal arrays, it's straightforward. If it should work with `const fn` results (e.g., algorithmic generation), the compile-time evaluator needs to handle record construction.

3. **Mini-notation interop**: Should `pat("c4 e4").events` return the *same* record shape as what `pattern()` accepts? Perfect round-tripping is valuable but means exposing internal details like `type_id` and `source_offset`.

4. **Filter semantics**: If you filter events out of a pattern, what happens to the timing? Do remaining events keep their absolute times (leaving gaps) or redistribute (closing gaps)? Both are useful — probably need both modes.

5. **Polyphonic events**: Chords produce multi-value events (`num_values > 1`). How should these appear in the event record? Array-valued `note` field? Multiple records at the same time? This affects how `map` over events interacts with chords.

6. **Naming**: `pattern()` vs `seq()` vs `events_to_pat()`? `.events` vs `.to_array()` vs `.decompose()`?

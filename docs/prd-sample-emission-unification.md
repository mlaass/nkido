> **Status: NOT STARTED** — SAMPLE_PLAY codegen wiring is duplicated across 6 class-method call sites, 1 static-helper inline mirror, and a separate SequenceCompiler property-extraction path. The two most recent sample fixes (`b4aeef4` lexer record-suffix, `05c1150` velocity post-MUL) each had to touch all three paths. A header comment at `codegen_patterns.cpp:1198-1208` explicitly warns implementers to keep the duplicates in sync. This PRD unifies them.

# PRD: Unify SAMPLE_PLAY Codegen Emission

## 1. Executive Summary

The compiler emits `SAMPLE_PLAY` for sample patterns from **eight distinct sites** in `akkado/src/codegen_patterns.cpp`, plus the SequenceCompiler builds pattern events from MiniAtomData via **two non-overlapping property-extraction paths** (`compile_atom_event` for non-polyrhythm atoms, `flatten_to_timelines` for polyrhythm atoms). Property handling for `{vel:V, dur:D, cutoff:V, ...}` lives in three places, and the SAMPLE_PLAY emission lives in seven. A pre-existing comment in the source warns to keep the duplicates synchronized.

This PRD proposes unifying all sample-pattern emission into a **single canonical free function** (`emit_sample_chain`) and a **single property-extraction helper** (`apply_atom_properties`) that both `compile_atom_event` and `flatten_to_timelines` consume. The change is **bit-identical at the bytecode level** — all existing tests pass unchanged — and is protected by a new **golden bytecode characterization test** that captures the emitted instruction sequence for ~10 representative sample-pattern shapes before the refactor and re-asserts them afterwards.

Key design decisions:

- **Free function, not a class member.** The static helper `emit_pattern_with_state` cannot call class methods, which is the entire reason the inline mirror exists today. A free function in the anonymous namespace, taking `BufferAllocator&` and an `emit` callback, is callable from both class methods and the static helper.
- **Opaque context struct.** `SamplePatternEmitCtx { state_id, value_buf, trigger_buf, velocity_buf, loc }` is passed by const-ref to keep the call sites narrow and to make future additions (e.g., per-voice velocity capture) one-line changes everywhere.
- **In-scope: scalar `sample()` builtin.** The scalar `sample(trig, pitch, "bd")` builtin emits SAMPLE_PLAY through the generic builtin-codegen path with different inputs (no SequenceState link, trigger from arg). The unified helper accepts a discriminator so both the pattern path and the scalar path land on the same emission code.
- **Custom property slots fixed on the polyrhythm path.** Currently `cutoff`, `bend`, `aftertouch` etc. are silently dropped inside `[a, b{cutoff:0.3}]`. The refactor's shared `apply_atom_properties` helper propagates every recognized slot uniformly.
- **Phased rollout: 4 commits.** Helper + golden test → migrate class call sites → migrate SequenceCompiler polyrhythm path → delete inline mirror. Each commit independently reviewable.

Out of scope: `SAMPLE_PLAY_LOOP` (the looping sampler used by `sample_loop()`), the `op_sample_play` opcode signature itself, performance optimizations.

---

## 2. Problem Statement

### 2.1 Current vs Proposed Behavior

| Aspect | Current | Proposed |
|--------|---------|----------|
| **SAMPLE_PLAY emission sites** | 7 (6 class methods + 1 static helper inline mirror) | 1 free function |
| **Property extraction (vel/dur/custom)** | 2 paths: `compile_atom_event`, `flatten_to_timelines` (latter only handles vel/dur) | 1 helper `apply_atom_properties`, fully covers custom slots |
| **Adding a new SAMPLE_PLAY input** | Requires edits at 7+ sites; the pre-existing `// Keep both in sync` comment is the only guardrail | One edit in `emit_sample_chain` + golden test regenerate |
| **Adding a new property kind on patterns** | Requires edits at 3 sites or one path silently drops it | One edit in `apply_atom_properties` |
| **Polyrhythm-internal `{cutoff:0.3}`** | Silently dropped (BranchEvent has no slot fields) | Propagated to merged event |
| **Scalar `sample()` builtin** | Emits SAMPLE_PLAY via generic builtin codegen; diverges from pattern path | Calls same `emit_sample_chain` with scalar-mode discriminator |

### 2.2 Root Cause Analysis

Three independent decisions accumulated into the current shape:

**(A) Pattern-transform proliferation.** Each pattern transform (`pat`, chord patterns, `velocity()`, `bank()`, `variant()`, transport-clock) was added as its own `handle_*_call` method that compiles its inner pattern via `SequenceCompiler` and then must emit a SAMPLE_PLAY tail when `is_sample_pattern == true`. Each handler grew its own copy of the `if (is_sample_pattern) { emit_sampler_wrapper(...) }` block. The current `emit_sampler_wrapper` is class member-only, so callers must already be class members.

**(B) Static helper / class member split.** `emit_pattern_with_state` was extracted as a free static helper to share pattern-compile boilerplate across `every`, `fast`, `slow`, etc. (`codegen_patterns.cpp:2620+`). Because it can't call class members, its SAMPLE_PLAY emission was duplicated inline (`codegen_patterns.cpp:2776+`). The header comment at `codegen_patterns.cpp:1198-1208` documents this duplication and asks future maintainers to keep the two in sync — but this is the bug factory, not a fix.

**(C) BranchEvent vs Event split.** `SequenceCompiler::compile_atom_event` builds a `cedar::Event` directly from `MiniAtomData` and applies `{vel, dur, cutoff, ...}` properties at line 420-440. The polyrhythm path at `codegen_patterns.cpp:687+` builds an intermediate `BranchEvent` struct via `flatten_to_timelines`, then merges branches into a single Event in `compile_polyrhythm_events`. The properties application was originally implemented only in `compile_atom_event`; the polyrhythm path silently dropped everything. The recent fix (commit `05c1150`) added `vel` and `dur` to `flatten_to_timelines` — but custom slots are still dropped, and now the same property logic exists in two places.

### 2.3 Concrete Evidence: Recent Bug History

| Commit | Fix | Sites that needed editing | Sites silently missed |
|--------|-----|---------------------------|----------------------|
| `b4aeef4` | Lexer accepts `{vel:V}` after sample tokens and chord symbols | `mini_lexer.cpp` (2 sites: `lex_sample_only`, `try_lex_chord_symbol`) | None — fix was lexer-only |
| `05c1150` (Bug B) | SAMPLE_PLAY output post-multiplied by velocity_buf | `emit_sampler_wrapper` + 6 call sites + inline mirror = **8 edits** | None caught, but only because every site was located by grep |
| `05c1150` (Bug A) | Polyrhythm flatten propagates `vel`/`dur` properties | `flatten_to_timelines` + `compile_polyrhythm_events` merge | **Custom property slots (cutoff/bend/aftertouch) still dropped** — outside the velocity fix scope |

**Two bugs in the same patch series, three different paths edited, custom slots still broken on the polyrhythm path.** That is the smell this refactor addresses.

### 2.4 Existing Infrastructure to Build On

| Component | Location | Reuse |
|-----------|----------|-------|
| `emit_sampler_wrapper` | `akkado/src/codegen_patterns.cpp:1202`, decl `akkado/include/akkado/codegen.hpp:433` | **Replace** with free `emit_sample_chain` |
| Inline mirror in `emit_pattern_with_state` | `codegen_patterns.cpp:2776-2825` | **Delete**; both paths call the new helper |
| `compile_atom_event` property loop | `codegen_patterns.cpp:420-441` | **Extract** body into `apply_atom_properties` helper |
| `flatten_to_timelines` MiniAtom case | `codegen_patterns.cpp:792-829` | **Replace** ad-hoc property loop with call to `apply_atom_properties` |
| `BranchEvent` struct | `codegen_patterns.cpp:633-645` | **Extend** with `prop_vals` array to carry custom slots through the merge; alternative considered and rejected (see §6) |
| `SamplePatternEmitCtx` | NEW | Bundle state_id / value_buf / trigger_buf / velocity_buf / loc |
| Scalar `sample()` builtin | `akkado/src/codegen.cpp:1067` (resolution); generic builtin emission via `BUILTINS["sample"]` in `akkado/include/akkado/builtins.hpp:315` | **Route through** `emit_sample_chain` with `kind = Scalar` |

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1:** All SAMPLE_PLAY (and the post-MUL velocity scaling) bytecode is emitted from exactly one function.
- **G2:** All `MiniAtomData → cedar::Event` property extraction lives in exactly one helper.
- **G3:** Bytecode emitted for every existing test case is **byte-identical** before and after the refactor. Existing test suites (akkado, cedar, nkido_cli, web Vitest) pass with no changes.
- **G4:** A new golden-bytecode characterization test asserts the emitted instruction sequence for ≥ 10 representative sample-pattern shapes (bare `s"bd"`, polyrhythm, chord-form, `velocity()`-scaled, `bank()`-routed, `variant()`-routed, `fast()`-transformed, transport-clock, scalar `sample()`, polyrhythm with `{cutoff:V}` after the custom-slot fix lands).
- **G5:** Custom property slots (cutoff, bend, aftertouch, etc.) propagate uniformly through both `compile_atom_event` and the polyrhythm flatten path. Each slot reaches `cedar::Event.prop_vals[slot]` regardless of pattern shape.
- **G6:** The `// Keep both in sync` comment block is removed; a single inline reference replaces the duplication warning.
- **G7:** Phased rollout with 4 reviewable commits, each independently reverting cleanly.

### 3.2 Non-Goals

- **`op_sample_play` opcode signature changes.** The opcode keeps its 5-input layout. Velocity continues to be applied as a post-MUL in codegen.
- **`SAMPLE_PLAY_LOOP`.** Sustained/looping samples (used by `sample_loop()`) are out of scope; same shape of cleanup may apply later but is a separate PRD.
- **Performance optimization.** No deduping of `PUSH_CONST 1.0` pitch buffers across patterns, no buffer reuse across patterns. Equivalent bytecode = identical instruction count.
- **`SequenceCompiler` event-merging logic.** The polyrhythm `min(velocity)` and per-slot value packing semantics introduced in `05c1150` are unchanged.
- **Behavioral changes.** No new user-visible features. Custom-slot propagation through polyrhythms is the only behavior change, and only because it was silently broken before — see §10 edge case E5.

---

## 4. Target Architecture

### 4.1 Caller Topology

```
                          ┌─────────────────────────────────┐
                          │   SamplePatternEmitCtx          │
                          │   { state_id, value_buf,        │
                          │     trigger_buf, velocity_buf,  │
                          │     loc, kind }                 │
                          └──────────────┬──────────────────┘
                                         │
                                         ▼
                          ┌────────────────────────────────────┐
                          │  emit_sample_chain(BufferAllocator&,│
                          │      EmitFn, ctx) -> uint16_t      │
                          │  PUSH_CONST(pitch=1.0) +           │
                          │  SAMPLE_PLAY +                     │
                          │  MUL(out, velocity_buf)            │
                          └──────────────┬─────────────────────┘
              ┌──────────────────────────┼──────────────────────────┐
              │                          │                          │
              ▼                          ▼                          ▼
   handle_mini_literal      emit_pattern_with_state      handle_velocity_call
   handle_chord_call        (used by every/fast/slow)    handle_bank_call
   transport-clock variant                               handle_variant_call
                                                         scalar sample() builtin
                                                         (via routing in codegen.cpp)
```

```
                          ┌──────────────────────────────────┐
                          │  apply_atom_properties(           │
                          │      const MiniAtomData&,         │
                          │      cedar::Event& out,            │
                          │      float t_span,                 │
                          │      PropertySlotAllocator& slots) │
                          └──────────────┬───────────────────┘
                  ┌──────────────────────┴──────────────────────┐
                  │                                             │
                  ▼                                             ▼
   SequenceCompiler::                       SequenceCompiler::flatten_to_timelines
   compile_atom_event                       (BranchEvent now stores prop_vals[]
                                            via the same helper)
```

### 4.2 SamplePatternEmitCtx

```cpp
struct SamplePatternEmitCtx {
    enum class Kind {
        Pattern,  // pat / chord / velocity / bank / variant / transport-clock
        Scalar,   // builtin sample(trig, pitch, "bd")
    };
    Kind            kind            = Kind::Pattern;
    std::uint32_t   seq_state_id    = 0;            // 0 in Scalar mode
    std::uint16_t   value_buf       = BUFFER_UNUSED; // sample-id buffer
    std::uint16_t   trigger_buf     = BUFFER_UNUSED;
    std::uint16_t   velocity_buf    = BUFFER_UNUSED; // BUFFER_UNUSED → emit no MUL
    std::uint16_t   pitch_buf       = BUFFER_UNUSED; // BUFFER_UNUSED → helper allocates PUSH_CONST 1.0
    SourceLocation  loc             = {};
};
```

### 4.3 emit_sample_chain (pseudocode)

```cpp
// In akkado/src/codegen_patterns.cpp anonymous namespace.
// Replaces CodeGenerator::emit_sampler_wrapper and the inline mirror in
// emit_pattern_with_state. Returns the final audio output buffer.
//
// EmitFn is `void(*)(std::vector<cedar::Instruction>&, const cedar::Instruction&)`
// to keep both class-member callers (using `emit(inst)`) and the static-helper
// caller (which already passes an emit_fn) on a single signature.
static std::uint16_t emit_sample_chain(
        BufferAllocator& buffers,
        std::vector<cedar::Instruction>& instructions,
        EmitFn emit_fn,
        const SamplePatternEmitCtx& ctx,
        std::function<void(SourceLocation, std::string_view)> error) {

    // 1. Pitch input — caller may have one already (scalar sample() supplies
    //    its 2nd arg); else allocate and emit PUSH_CONST 1.0.
    std::uint16_t pitch_buf = ctx.pitch_buf;
    if (pitch_buf == BUFFER_UNUSED) {
        pitch_buf = buffers.allocate();
        if (pitch_buf == BUFFER_UNUSED) { error(ctx.loc, "E101"); return BUFFER_UNUSED; }
        cedar::Instruction pi{};
        pi.opcode = cedar::Opcode::PUSH_CONST;
        pi.out_buffer = pitch_buf;
        std::fill(std::begin(pi.inputs), std::end(pi.inputs), 0xFFFFu);
        encode_const_value(pi, 1.0f);
        emit_fn(instructions, pi);
    }

    // 2. SAMPLE_PLAY. inputs[3]/[4] carry the linked SequenceState id (split
    //    16-bit halves) in Pattern mode; both BUFFER_UNUSED in Scalar mode so
    //    the runtime falls back to the scalar sample_id buffer.
    std::uint16_t output_buf = buffers.allocate();
    if (output_buf == BUFFER_UNUSED) { error(ctx.loc, "E101"); return BUFFER_UNUSED; }
    cedar::Instruction si{};
    si.opcode = cedar::Opcode::SAMPLE_PLAY;
    si.out_buffer = output_buf;
    si.inputs[0] = ctx.trigger_buf;
    si.inputs[1] = pitch_buf;
    si.inputs[2] = ctx.value_buf;
    if (ctx.kind == Kind::Pattern) {
        si.inputs[3] = static_cast<std::uint16_t>(ctx.seq_state_id & 0xFFFFu);
        si.inputs[4] = static_cast<std::uint16_t>((ctx.seq_state_id >> 16) & 0xFFFFu);
        si.state_id  = ctx.seq_state_id + 1;
    } else {
        si.inputs[3] = BUFFER_UNUSED;
        si.inputs[4] = BUFFER_UNUSED;
        si.state_id  = ctx.seq_state_id;  // caller supplies a unique scalar state id
    }
    emit_fn(instructions, si);

    // 3. Velocity post-multiply. Skip if velocity_buf is BUFFER_UNUSED — the
    //    scalar sample() path has no velocity input today, so it returns the
    //    raw output. Future extension: scalar sample() gains a velocity arg.
    if (ctx.velocity_buf == BUFFER_UNUSED) return output_buf;

    std::uint16_t scaled_buf = buffers.allocate();
    if (scaled_buf == BUFFER_UNUSED) { error(ctx.loc, "E101"); return BUFFER_UNUSED; }
    cedar::Instruction mi{};
    mi.opcode = cedar::Opcode::MUL;
    mi.out_buffer = scaled_buf;
    mi.inputs[0] = output_buf;
    mi.inputs[1] = ctx.velocity_buf;
    std::fill(mi.inputs + 2, std::end(mi.inputs), 0xFFFFu);
    emit_fn(instructions, mi);
    return scaled_buf;
}
```

### 4.4 apply_atom_properties (pseudocode)

```cpp
// Single source of truth for {vel, dur, cutoff, bend, ...} on sample atoms.
// Caller decides whether to use the result on a `cedar::Event` directly
// (compile_atom_event) or unpack its fields onto a BranchEvent
// (flatten_to_timelines). Custom-slot allocation is delegated to the slot
// allocator owned by SequenceCompiler.
struct AtomPropertiesOut {
    float velocity     = 1.0f;
    float duration_mul = 1.0f;  // multiplied by t_span in caller
    std::array<float, MAX_PROP_SLOTS> prop_vals{};
    std::uint8_t prop_vals_used = 0;  // bitmap of populated slots
};

static AtomPropertiesOut apply_atom_properties(
        const MiniAtomData& ad,
        PropertySlotAllocator& slots) {
    AtomPropertiesOut out;
    out.velocity = ad.velocity;
    for (const auto& [key, value] : ad.properties) {
        if (key == "vel") {
            out.velocity = std::clamp(value, 0.0f, 1.0f);
        } else if (key == "dur") {
            if (value > 0.0f) out.duration_mul = value;
        } else {
            int slot = slots.allocate(key);
            if (slot >= 0 && slot < MAX_PROP_SLOTS) {
                out.prop_vals[slot] = value;
                out.prop_vals_used |= (1u << slot);
            }
        }
    }
    return out;
}
```

### 4.5 BranchEvent extension

```cpp
struct BranchEvent {
    float          time;
    float          duration;
    float          velocity;
    std::uint16_t  type_id;
    std::uint16_t  source_offset;
    std::uint16_t  source_length;
    bool           is_rest;
    std::uint32_t  sample_id;
    std::string    sample_name;
    std::string    sample_bank;
    std::uint8_t   sample_variant;
    // NEW: carry custom property slots through polyrhythm merge.
    // Indexed by slot id from PropertySlotAllocator. When two branches both
    // populate the same slot at the same time, the merge takes the first
    // non-zero value (mirrors current `velocity` and `type_id` semantics).
    std::array<float, MAX_PROP_SLOTS> prop_vals{};
    std::uint8_t   prop_vals_used = 0;
};
```

---

## 5. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `op_sample_play` (cedar opcode) | **Stays** | No signature change. Still 5 inputs. |
| `op_sample_play_loop` | **Stays** | Out of scope; not used by patterns. |
| `MUL` opcode | **Stays** | Already used for velocity post-multiply since `05c1150`. |
| Bytecode format / Instruction layout | **Stays** | Bit-identical for all existing test inputs. |
| Existing test suites (akkado, cedar, nkido_cli, web Vitest) | **Stays** | All pass unchanged. Required by G3. |
| `CodeGenerator::emit_sampler_wrapper` | **Removed** | Replaced by `emit_sample_chain`. |
| Inline mirror in `emit_pattern_with_state` | **Removed** | Replaced by `emit_sample_chain` call. |
| `compile_atom_event` property loop | **Modified** | Body extracted into `apply_atom_properties`. |
| `flatten_to_timelines` MiniAtom case | **Modified** | Calls `apply_atom_properties`; populates `BranchEvent.prop_vals`. |
| `compile_polyrhythm_events` merge | **Modified** | Merges `prop_vals` from `BranchEvent` into the merged `cedar::Event`. |
| `BranchEvent` struct | **Modified** | Adds `prop_vals` + `prop_vals_used`. |
| `emit_sample_chain` (free function) | **New** | Single SAMPLE_PLAY emission point. |
| `apply_atom_properties` (free function) | **New** | Single property-extraction helper. |
| `SamplePatternEmitCtx` struct | **New** | Argument bundle for `emit_sample_chain`. |
| Golden bytecode test | **New** | `akkado/tests/test_sample_emission_golden.cpp` (or extend `test_codegen.cpp`). |
| Scalar `sample()` builtin codegen | **Modified** | Routes through `emit_sample_chain` with `Kind::Scalar`. Bytecode unchanged. |
| `// Keep both in sync` comment | **Removed** | No longer applicable. |

---

## 6. File-Level Changes

| File | Change | Phase |
|------|--------|-------|
| `akkado/src/codegen_patterns.cpp` | Add `SamplePatternEmitCtx`, `apply_atom_properties`, `emit_sample_chain` in anonymous namespace | 1 |
| `akkado/tests/test_sample_emission_golden.cpp` | New: golden-bytecode tests for ≥ 10 sample-pattern shapes | 1 |
| `akkado/tests/CMakeLists.txt` | Wire new test file | 1 |
| `akkado/src/codegen_patterns.cpp` | Migrate the 6 class-method `emit_sampler_wrapper` call sites (handle_mini_literal, handle_chord_call, handle_velocity_call, handle_bank_call, handle_variant_call, transport-clock variant) to `emit_sample_chain` | 2 |
| `akkado/src/codegen.cpp` | Route scalar `sample()` builtin emission through `emit_sample_chain` (Kind::Scalar) | 2 |
| `akkado/include/akkado/codegen.hpp` | Remove `emit_sampler_wrapper` declaration | 2 (final cleanup) or 4 |
| `akkado/src/codegen_patterns.cpp` | Migrate `compile_atom_event` and `flatten_to_timelines` to `apply_atom_properties`; extend `BranchEvent` with `prop_vals`; merge `prop_vals` in `compile_polyrhythm_events` | 3 |
| `akkado/src/codegen_patterns.cpp` | Migrate `emit_pattern_with_state` static helper to call `emit_sample_chain`; delete inline mirror; remove the `// Keep both in sync` comment block at lines 1198-1208 | 4 |

Files **explicitly NOT touched**:

- `cedar/include/cedar/opcodes/samplers.hpp` — opcode unchanged
- `cedar/include/cedar/vm/instruction.hpp` — bytecode format unchanged
- `web/wasm/nkido_wasm.cpp` — WASM bindings unchanged
- `web/static/worklet/cedar-processor.js` — runtime unchanged
- `nkido-vscode/**` — extension unchanged

---

## 7. Implementation Phases

Each phase is a single commit, independently reviewable, leaves the tree green.

### Phase 1 — Introduce helpers + golden test (no behavior change)

**Goal:** Add `SamplePatternEmitCtx`, `emit_sample_chain`, `apply_atom_properties` in the anonymous namespace alongside the existing `emit_sampler_wrapper`. No call sites migrated yet. Add the golden-bytecode test that captures current bytecode for the representative pattern suite — this is the safety net for phases 2-4.

**Files:**
- `akkado/src/codegen_patterns.cpp` (new helpers)
- `akkado/tests/test_sample_emission_golden.cpp` (new test)
- `akkado/tests/CMakeLists.txt` (wire test)

**Verification:**
- Build clean: `cmake --build build --target akkado_tests`
- New golden test passes against current code (since no call sites use the new helpers yet, this baselines current bytecode).
- All existing suites still pass.

### Phase 2 — Migrate class-method call sites + scalar sample()

**Goal:** Replace each of the 6 `emit_sampler_wrapper(...)` calls in class methods with `emit_sample_chain(buffers_, instructions_, &CodeGenerator::emit_static, ctx)` (or equivalent thunk). Route the scalar `sample()` builtin through `emit_sample_chain` with `Kind::Scalar`.

**Files:**
- `akkado/src/codegen_patterns.cpp` (6 call sites)
- `akkado/src/codegen.cpp` (scalar sample() routing)

**Verification:**
- Golden bytecode test passes — proves bit-identical bytecode for every migrated site.
- All existing suites pass.

### Phase 3 — Unify SequenceCompiler property handling

**Goal:** Extract the property loop in `compile_atom_event` into `apply_atom_properties`. Migrate `flatten_to_timelines` to call the same helper. Extend `BranchEvent` with `prop_vals` + `prop_vals_used`. Update `compile_polyrhythm_events` merge to copy `prop_vals` onto the emitted `cedar::Event`.

This phase changes one observable behavior: custom property slots (cutoff, bend, etc.) that were previously dropped on the polyrhythm path now propagate. Add a targeted akkado test for `pat("[hh, bd{cutoff:0.3}]")` asserting the merged event's `prop_vals[cutoff_slot] == 0.3`.

**Files:**
- `akkado/src/codegen_patterns.cpp` (BranchEvent, compile_atom_event, flatten_to_timelines, compile_polyrhythm_events)
- `akkado/tests/test_codegen.cpp` (new polyrhythm custom-slot test)

**Verification:**
- Golden bytecode test passes for shapes that don't use custom slots in polyrhythms.
- The new polyrhythm-custom-slot test passes.
- Update the golden test to include `[hh, bd{cutoff:0.3}]` and capture its post-fix bytecode.
- All existing suites pass.

### Phase 4 — Delete the inline mirror

**Goal:** Migrate `emit_pattern_with_state` (the static helper used by `every`, `fast`, `slow`, …) to call `emit_sample_chain` instead of its inline copy. Delete the inline mirror block and the `// Keep both in sync` warning comment at `codegen_patterns.cpp:1198-1208`. Delete `CodeGenerator::emit_sampler_wrapper` declaration from the header and definition from the cpp file.

**Files:**
- `akkado/src/codegen_patterns.cpp` (delete inline mirror, delete `emit_sampler_wrapper` definition)
- `akkado/include/akkado/codegen.hpp` (delete `emit_sampler_wrapper` declaration)

**Verification:**
- Golden bytecode test passes — proves bit-identical bytecode after the final consolidation.
- All existing suites pass.
- Grep for `SAMPLE_PLAY` in `akkado/src/` and `akkado/include/` returns exactly one emission site (the new helper).

---

## 8. Testing / Verification Strategy

### 8.1 Golden Bytecode Characterization Test (new)

`akkado/tests/test_sample_emission_golden.cpp` runs `akkado::compile()` on each pattern below, decodes the resulting bytecode, and asserts the (opcode, output_buf, input_bufs[0..4], state_id_low_16) tuples match a checked-in golden array.

| ID | Pattern | Why |
|----|---------|-----|
| G1 | `s"bd ~ ~ ~".out()` | Bare sample pattern (handle_mini_literal) |
| G2 | `s"[hh,bd] ~ ~ ~".out()` | Polyrhythm path |
| G3 | `s"bd{vel:0.25} ~ ~ ~".out()` | Property propagation, single atom |
| G4 | `s"[hh,bd{vel:0.25}] ~ ~ ~".out()` | Property propagation through polyrhythm |
| G5 | `velocity(s"bd ~ ~ ~", 0.5).out()` | handle_velocity_call site |
| G6 | `s"bd".bank("X")` (with stub bank) | handle_bank_call site |
| G7 | `s"bd".variant(2)` | handle_variant_call site |
| G8 | `s"bd ~".fast(2).out()` | Goes through emit_pattern_with_state inline mirror |
| G9 | `pat("c4 e4 g4") |> chord(%)` (chord-form) | handle_chord_call sample-mode path |
| G10 | `sample(t, 1, "bd")` (scalar) | Scalar sample() builtin path |
| G11 | (Phase 3+) `s"[hh,bd{cutoff:0.3}]"` | Custom slot through polyrhythm |

Golden values are checked in. If a phase changes behavior unintentionally, the test fails with the exact opcode/buffer that drifted. The Phase 3 commit explicitly updates G11's expected bytecode (the only intentional change).

### 8.2 Regression Coverage from Existing Suites

- **akkado_tests** — 583 cases, includes the 4 `[velocity]` tests added in `05c1150`. All pass unchanged.
- **cedar_tests** — 184 cases. All pass unchanged.
- **nkido_cli_tests** — 7 cases including the 4 `[serve][load_bank]` tests added in `05c1150`. All pass unchanged.
- **web Vitest sample-velocity** — 3 cases. Passes against the rebuilt WASM artifact.

### 8.3 Manual smoke test

After Phase 4, render `web/static/patches/test.akk` (the user's pattern from the velocity bug) via `nkido-cli render` and confirm step-5 peak amplitude is unchanged from the pre-refactor render. (Not automated because it requires the bank manifest fixture, but a one-line shell command at PR review time.)

---

## 9. Risks

| Risk | Mitigation |
|------|------------|
| **Bytecode drift on subtle ordering** (e.g., buffer allocation order changes when callers go through one helper) | Golden test fails loudly; inspect drift case-by-case. Buffer allocation order is deterministic (sequential allocator) so as long as the helper allocates in the same order as the inline copy did, the golden holds. |
| **EmitFn callback overhead in hot paths** | Codegen is not a hot path (per-compile, not per-block). Function-pointer overhead is negligible. |
| **Phase 3 changes observable behavior** (custom slots through polyrhythm) | Behavior change is intentional and documented (currently silently broken). Targeted test pins the new behavior. Golden test for G11 added in Phase 3 to capture the new bytecode. |
| **emit_pattern_with_state has additional callers I missed** | Phase 1 grep + golden test for G8 (fast()) catches mismatches. |
| **Scalar sample() routing changes its bytecode** | Phase 2 golden test for G10 catches any drift; the scalar path's emission today is the same shape as the pattern path's `if !pattern { ... }` branch in `emit_sample_chain`. |
| **Reviewers cannot bisect cleanly** | 4-commit phasing with each commit independently buildable+testable. Each commit message states which sites it migrates. |
| **Future contributors miss the new conventions** | Add a one-paragraph comment at the top of `emit_sample_chain` explaining the role and pointing back at this PRD. Replace the deleted `// Keep both in sync` block with a one-line `// All SAMPLE_PLAY emission goes through emit_sample_chain. See docs/prd-sample-emission-unification.md.` |

---

## 10. Edge Cases

### E1. Empty pattern (no events)

`pat("")` — the SequenceCompiler returns no events; `is_sample_pattern` is false; `emit_sample_chain` is never called. **No change.**

### E2. Rest-only polyrhythm `[~, ~]`

`flatten_to_timelines` produces BranchEvents with `is_rest=true`; `compile_polyrhythm_events` skips them; merged event count is zero. **No change.**

### E3. Polyrhythm where one branch has `{vel:0.25}` and another has `{cutoff:0.3}`

Phase 3 behavior:
- `apply_atom_properties` populates each branch's `prop_vals` independently.
- Merge takes `min(velocity)` (existing behavior, preserved).
- Merge takes the first non-zero value per property slot, mirroring the existing `type_id` / `source_offset` merge semantics.
- Result: the merged event has `velocity = 0.25` AND `prop_vals[cutoff_slot] = 0.3`.

This is the most subtle case the refactor enables. Captured in golden test G11+ and documented in the PRD as the **intentional behavior change** of Phase 3.

### E4. Scalar sample() with no velocity

`sample(trig, pitch, "bd")` — `Kind::Scalar`, `velocity_buf = BUFFER_UNUSED`, helper skips the post-MUL and returns the raw SAMPLE_PLAY output. Bit-identical to today.

### E5. Custom slot allocator collision

Two patterns in the same program both use `{cutoff:V}`. The slot allocator currently assigns the same slot (correct, by key). `apply_atom_properties` consults the same allocator instance so both patterns see the same slot ID. **No change.**

### E6. Bytecode format change between phases

If a phase introduces a buffer-allocation order change that drifts the golden, the test fails. The fix is either to preserve the order in the helper or to update the golden in the same commit (with an explicit commit-message line). Phase 3 is the only phase expected to drift the golden (G11 only).

### E7. emit_pattern_with_state called from a path I haven't enumerated

Confirmed callers: `every`, `fast`, `slow`, plus a transport-clock variant (already a class method, not a helper user). Phase 4 grep on `emit_pattern_with_state` is the verification. If a new caller surfaces during the migration, treat it the same way — the static helper now calls `emit_sample_chain`, so all of its callers are covered transitively.

### E8. Compile-time error path inside the helper (e.g., E101 buffer pool exhausted)

`emit_sample_chain` accepts an `error` callback so it can report buffer-allocator exhaustion via the caller's `error()` method. Class-method callers pass a thunk that calls `CodeGenerator::error`; the static helper passes a thunk that calls the static-helper's existing diagnostic mechanism (currently it just returns `void_val()` on exhaustion — preserve that behavior by routing through the existing pattern).

---

## 11. Success Criteria

The refactor is complete when:

1. ✅ `grep -n "Opcode::SAMPLE_PLAY" akkado/src/ akkado/include/` returns **exactly one** emission site, `emit_sample_chain`.
2. ✅ `grep -n "Mirrors\|Keep both in sync\|emit_sampler_wrapper" akkado/` returns **zero** results.
3. ✅ Property handling for `{vel, dur, cutoff, bend, ...}` lives in **exactly one** function, `apply_atom_properties`.
4. ✅ All existing test suites pass with no edits (akkado, cedar, nkido_cli, web Vitest).
5. ✅ The new golden bytecode test passes for all ≥ 10 representative shapes.
6. ✅ The new polyrhythm-custom-slot test (`[hh, bd{cutoff:0.3}]`) passes — fixes the silent-drop bug.
7. ✅ 4 commits land cleanly on master with no force-pushes; each commit independently builds and passes its own phase's verification.

---

## 12. Future Work (Out of Scope)

- **`SAMPLE_PLAY_LOOP` unification** — the looping sampler used by the scalar `sample_loop()` builtin has its own divergent voice mixing. A follow-up PRD can apply the same shape of cleanup once this one lands.
- **Per-voice velocity capture** — currently `velocity_buf` scales the whole sampler output for the entire block. Polyphonic sample tails that overlap meaningfully (rare in drum patterns) would benefit from velocity captured at trigger time. The opaque `SamplePatternEmitCtx` makes this a single-site change once the opcode supports it.
- **Bytecode interning / buffer reuse across patterns** — multiple sample patterns each allocate their own `PUSH_CONST 1.0` pitch buffer. A future pass can dedupe these. Out of scope here because `Bit-identical bytecode` is a constraint.
- **Performance profiling of the emit pipeline** — codegen has never shown up as a bottleneck. If it does, the unified helper makes it trivial to inline or specialize.

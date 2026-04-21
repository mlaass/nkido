# Audit: PRD Universal Stereo Signal Semantics in Akkado

**PRD:** `docs/prd-stereo-support.md`
**Audit base:** `5b8e508` (Refine stereo PRD: resolve OQ1, add stereo-generator category)
**Audit head:** `81d3a61` + uncommitted working tree (stereo-support implementation + audit fixes)
**Audited:** 2026-04-21

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| **G1** — every expression has a statically known channel count | Met | `ChannelCount` enum + `TypedValue.channels` / `.right_buffer` (`akkado/include/akkado/typed_value.hpp:26-115`). Stereo-producing handlers tag their results (`akkado/src/codegen_stereo.cpp` — `stereo`, `pan`, `width`, `ms_encode`, `ms_decode`, `pingpong`). Identifier resolution promotes `s = stereo(...)` bindings to `Stereo` (`akkado/src/codegen.cpp:500-507`). **Follow-up landed 2026-04-22**: the PRD §5.2 `BuiltinSignature` catalog is now declarative on `BuiltinInfo` — per-builtin `input_channels`, `output_channels`, `auto_lift` fields (`akkado/include/akkado/builtins.hpp:83-99`). Generic dispatch gates auto-lift on `builtin->auto_lift` (`akkado/src/codegen.cpp:1114-1123`) and populates the result's channel count from `builtin->output_channels` (`akkado/src/codegen.cpp:1390-1401`). A new E186 (`codegen.cpp:1063-1103`) emits a precise channel-type mismatch diagnostic for non-auto-lift builtins. |
| **G2** — mono DSP on stereo auto-lifts | Met | Detection moved out of the `requires_state` gate: `akkado/src/codegen.cpp:1016-1060` (stereo expansion detection runs unconditionally). Stateless ops also auto-lift — verified by `saturate` (DISTORT_TANH) and `softclip` (DISTORT_SOFT) in `akkado/tests/test_types.cpp:138-160`. VM dispatch wrapper at `cedar/src/vm/vm.cpp:467-480` handles the flag. |
| **G3** — `stereo(x)` and `mono(x)` are the canonical conversions | Met | `handle_stereo_call` at `akkado/src/codegen_stereo.cpp:152-203`, `handle_mono_call` at `akkado/src/codegen_stereo.cpp:51-148`. Both documented in `web/static/docs/reference/builtins/stereo.md` and `web/static/docs/concepts/signals.md`. |
| **G4** — compile-time errors for mono/stereo mismatches | Met | `mono(mono)` → E181 (`codegen_stereo.cpp:113`); `stereo(stereo)` → E182 (`codegen_stereo.cpp:171`); `left(mono)` → E183 (`codegen_stereo.cpp:233`); `right(mono)` → E184 (`codegen_stereo.cpp:263`); `out(stereo, mono)` / `out(mono, stereo)` → E185 (`codegen.cpp:983-1012`). Every error carries the argument's source location. All covered by `akkado/tests/test_types.cpp`. |
| **G5** — existing mono programs compile byte-identically and hot-swap compatibly | Met | Mono path `state_id` unchanged: it uses `fnv1a(semantic_path)`. Stereo auto-lift appends `/L` for the L pass; the VM XORs in `STEREO_STATE_XOR_R` for the R pass (`cedar/include/cedar/vm/instruction.hpp:208`). Full akkado test suite stays green (433/434; 1 pre-existing version-string failure unrelated to this PRD). |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build` | Pass | Warnings pre-existing in `wav_loader.hpp` and `codegen.cpp` record-literal path — not introduced by this work |
| `./build/akkado/tests/akkado_tests "[stereo]"` | Pass (12/12 tests, 99 assertions) | PRD §11.1 coverage |
| `./build/akkado/tests/akkado_tests "[types]"` | Pass (8/8 tests, 100 assertions) | New `test_types.cpp` — PRD §8 deliverable landed |
| `./build/akkado/tests/akkado_tests` | 433/434 | Single failure is `Version::patch == 0` assertion at `test_akkado.cpp:275` — pre-existing on base, unrelated |
| `./build/cedar/tests/cedar_tests` | 130/131 | Single failure is `Version::patch == 0` assertion at `test_cedar.cpp:43` — pre-existing on base, unrelated |
| `uv run python test_op_mono_downmix.py` | Pass | Verifies `(L+R)*0.5` across correlated/anti-phase/DC/uncorrelated cases |
| `uv run python test_op_stereo_input_flag.py` | Pass | Verifies per-channel state independence and L-channel bit-identity to plain mono run |
| `cd web && bun run build:docs` | Pass | 26 documents, 262 lookup entries, 26 previews regenerated |

## Findings

### Unmet Goals

*(None. G1 was closed 2026-04-22 by landing the declarative `BuiltinSignature` catalog — see Decisions Recorded.)*

### Stubs

*(None found in the diff.)*

### Regressions

*(None introduced by this PRD. Two pre-existing version-string failures persist on both `base` and `HEAD`.)*

### Missing Tests

*(All addressed during this audit — see Decisions Recorded.)*

### Scope Drift

- `docs/prd-cedar-esp32.md` was modified in commit `2790a75` between base and HEAD. This is unrelated refinement of a sibling PRD — not drift from the stereo-support implementation, but it sits on the same branch.
- ~~Auto-lift implementation is piggybacked on the chord-expansion multi-buffer detection rather than on a declarative `BuiltinSignature` catalog.~~ **Resolved 2026-04-22**: declarative `BuiltinSignature` landed on `BuiltinInfo`; auto-lift now gated on `builtin->auto_lift` with chord-expansion on a separate branch.

### Convention Drift

*(No documented project-convention rules violated.)*

### Suggestions

- The `BuiltinSignature` catalog (PRD §5.2) remains a reasonable follow-up. Declaring `auto_lift`/`input_channels`/`output_channels` per builtin would let us remove the `requires_state`/multi-buffer plumbing from the auto-lift path and give users more precise diagnostics for non-stereo-aware builtins.
- Consider renaming `fold` (array reduce) vs `fold` (DSP wavefolder) to disambiguate. Today the DSP wavefolder is shadowed by the array-fold handler, so the only way to reach the wavefolder is via the `wavefold` alias (which itself resolves to the array-fold handler — possibly broken, outside this PRD).

## Decisions Recorded

During the audit the user approved four fixes, which were landed:

1. **G2 — extend auto-lift to stateless opcodes.** Moved multi-buffer detection out of the `if (builtin->requires_state)` guard so stateless DSP ops (`saturate`, `softclip`, `distort_*`) auto-lift on stereo input. Chord-expansion-to-N still gates on `requires_state` as it always has. Evidence: `akkado/src/codegen.cpp:1016-1146`.
2. **G4 — add the missing stereo type-error paths.** Added E182/E183/E184 in `codegen_stereo.cpp` and E185 in `codegen.cpp` for `stereo(stereo)`, `left(mono)`, `right(mono)`, and `out(L|R, stereo)` respectively. Each error points to the problematic argument's source location.
3. **Tests — create `akkado/tests/test_types.cpp`.** Eight new `[types]` test cases cover: error paths (E181–E185), positive cases (left/right on stereo, stereo on mono), stateless auto-lift (`saturate`, `softclip`), and mono+stereo arithmetic broadcasting. Registered in `akkado/tests/CMakeLists.txt`.
4. **Docs — create `web/static/docs/concepts/signals.md` and update `docs/cedar-architecture.md`.** Concept page covers defaults, conversions, auto-lift, mixed-channel arithmetic, and links to the builtins reference. Architecture doc now describes the `flags` field and `STEREO_INPUT` dispatch.

User accepted:
- ~~**G1 — BuiltinSignature catalog deferred.**~~ **Resolved 2026-04-22**: the declarative `BuiltinSignature` catalog (per-builtin `input_channels`, `output_channels`, `auto_lift`) landed on `BuiltinInfo`. Generic dispatch now gates auto-lift on `builtin->auto_lift` (rather than inferring from buffer adjacency) and new error code E186 emits precise channel-type mismatches for non-lift builtins. 32 DSP builtins annotated as `auto_lift = true`; 8 stereo-in builtins annotated with `input_channels[0] = Stereo` for self-documentation. Full suite: 435/436 akkado (2 new [types] cases), 130/131 cedar (pre-existing failures only).

Supporting side-effect:
- **Identifier resolution promotes stereo bindings.** `codegen.cpp:500-507` checks `stereo_buffer_pairs_` when resolving a Variable/Parameter symbol so `s = stereo(...)` references round-trip through the channel-type system. This was required to make `out(saw(220), s)` correctly emit E185.
- **Binary-op broadcasting recognises stereo.** `codegen_arrays.cpp:866-880` registers a 2-buffer broadcast result as Stereo (rather than Array-of-2) when at least one operand was a stereo signal with adjacent output buffers. Enables `dry * 0.3 + stereo_wet * 0.7 |> out(%)` to compile.

## PRD Status

- Before: `PROPOSED`
- After: `PROPOSED` (unchanged — **pending user confirmation**; see next AskUserQuestion in the audit session)

## Recommended Next Steps

- ~~Land a declarative `BuiltinSignature` (PRD §5.2) so channel-type enforcement doesn't rely on the chord-expansion plumbing.~~ **Done 2026-04-22.**
- Disambiguate `fold` (array) vs `fold` (wavefolder) or document that the DSP wavefolder is reached via another name.
- Investigate the two pre-existing version-string test failures (`test_akkado.cpp:275`, `test_cedar.cpp:43`) so the suites can ship all-green.
- Follow-up cleanup: once `TypedValue.channels` propagation covers every pipe/binding path, remove the legacy `stereo_buffer_pairs_`, `stereo_outputs_`, and `param_multi_buffer_sources_` fallbacks. Today they're retained as a belt-and-braces tier so auto-lift still triggers when channel info hasn't yet propagated through a `let` binding or user-function parameter.

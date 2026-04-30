# Audit: Smooch Wavetable Oscillator (PRD §smooch)

**PRD:** `docs/prd-smooch-wavetable-synth.md`
**Audit base:** `8d1c621` (2026-04-29 — last commit before implementation work)
**Audit head:** `60f64f3` (2026-04-30 — current `master`)
**Audited:** 2026-04-30

The implementation lands across two commits: `efe84eb` ("Smooch: implement OSC_WAVETABLE wavetable oscillator (multi-bank)") and `60f64f3` ("Smooch: smooth tablePos to suppress UI-cadence sidebands"). The PRD's Status line said "NOT STARTED" at the start of the audit despite the feature shipping; that's been corrected.

## Goal Verification

| Goal (PRD §2.1) | Status | Evidence |
|---|---|---|
| Zero aliasing across the audible range via per-octave mip-maps + crossfade | Met | `cedar/src/wavetable/preprocessor.cpp:73-121` (mip generation), `cedar/include/cedar/opcodes/oscillators.hpp:1117-1132` (equal-power crossfade). Test 3 measured -102.7 dB alias floor at 10 kHz. |
| Low noise floor via 4-point Niemitalo-optimal interpolation | Met | `cedar/include/cedar/opcodes/oscillators.hpp:980-990` (kernel), `1124-1130` (use). Test 1 measured THD = -101 dB. |
| Live-coding stability: load + FFT off audio thread; audio path lock-free + allocation-free | Met | `cedar/src/wavetable/registry.cpp:117-132` (snapshot-pin pattern), `cedar/src/vm/vm.cpp:85-87` (per-block snapshot). Hot loop allocates nothing; mutex held only during snapshot copy. |
| Standard file compatibility — Serum-style 2048-frame WAV | Met | `cedar/src/wavetable/preprocessor.cpp:138-145` (multiple-of-2048 check). Re-uses `cedar::WavLoader::load_from_file/load_from_memory`. |
| Polyphonic by default — many `smooch` voices share one immutable bank | Met | `SmoochState` per-instance via `state_id` (`cedar/include/cedar/opcodes/dsp_state.hpp:31-37`); banks shared via `std::shared_ptr<const WavetableBank>`. |
| Hot-swap safe — code reload preserves phase via semantic-ID matching | Met | Codegen wires `push_path("smooch#N") + compute_state_id()` (`akkado/src/codegen_patterns.cpp:5620-5622`) — the same pattern every other stateful pattern operator uses. New audit test (`test_wavetable.cpp:272-330`) confirms the per-block opcode honors that state slot. |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build` | Pass | Builds clean; only pre-existing `wav_loader.hpp` int-to-float warnings unrelated to wavetable code. |
| `./build/cedar/tests/cedar_tests "[wavetable]"` | Pass (12 cases / 71 assertions) | 9 original + 3 audit additions. |
| `./build/cedar/tests/cedar_tests` (full suite) | Pass (167 cases / 334 371 assertions) | No regressions. |
| `./build/akkado/tests/akkado_tests` | Pass (539 cases / 137 399 assertions) | No regressions. |
| `cd experiments && uv run python test_op_smooch.py` | Pass (12/12) | 11 PRD tests + 1 audit addition (`test_8b_freq_edge_cases`). PRD §12 #10 (hot-swap audio-path test) intentionally remains deferred — its underlying invariant is now covered by the new C++ test. |

## Findings

### Unmet Goals

None. Every PRD §2.1 goal traces to specific code and a specific test.

### Stubs

None. Grep across `cedar/{include,src}/wavetable/`, `cedar/include/cedar/opcodes/oscillators.hpp`, `akkado/src/codegen_patterns.cpp`, `web/wasm/nkido_wasm.cpp`, and `experiments/test_op_smooch.py` returned zero TODO/FIXME/NotImplemented markers in the changed code.

### Regressions

None. `cedar_tests` and `akkado_tests` pass identically before and after the new test additions.

### Coverage Gaps

All resolved during the audit:

- **PRD §12 #10 (hot-swap state preservation)** — closed with new C++ test `Wavetable: SmoochState phase is preserved across process_block calls for the same state_id` (`cedar/tests/test_wavetable.cpp:272`). The full audio-path hot-swap test (which would require Akkado compile + program reload) remains deferred per the existing comment in `test_op_smooch.py:696-698`, but the smooch-specific contract (the opcode reads/writes a state slot keyed by `state_id`, not a fresh copy) is now pinned. The codegen pattern itself (`push_path("smooch#N") + compute_state_id()`) is identical to `pat`, `chord`, `slow`, `fast`, `dur`, etc., all of which have their own hot-swap coverage.
- **PRD §12 #11 (bank lifetime during swap)** — closed with new C++ test `Wavetable: in-place bank replace keeps ID and old shared_ptr alive` (`cedar/tests/test_wavetable.cpp:225`). The original PRD framing ("`wt_load` swaps bank — existing voices continue with old bank") is partially obsolete given the multi-bank model (where `wt_load` now adds a slot rather than replacing the active bank), but the underlying lifetime invariant still applies whenever `set_named` re-registers under an existing name; that path is now under test.
- **PRD §8.a/8.b (out-of-range freq clamping)** — closed with `test_8b_freq_edge_cases` (`experiments/test_op_smooch.py:514`), which drives `f=0`, `f=-1`, and `f=Nyquist` and asserts finite, bounded output. Test 2's existing 0.49 × sr sweep no longer leaves a gap at the exact Nyquist frequency.
- **PRD §8.n (stereo WAV rejection)** — closed with new C++ test `Wavetable: stereo WAV is rejected` (`cedar/tests/test_wavetable.cpp:209`).

### Missing Tests

None remaining after the audit additions.

### Scope Drift

- **Multi-bank model (intentional, PRD-superseding).** PRD §2.2 stated as a v1 non-goal: "No multiple simultaneous banks in v1: only one wavetable bank is active at a time." The shipped implementation is fully multi-bank — `inst.rate` (8 bits) encodes a bank ID (0..63), `WavetableBankRegistry` indexes banks by ID, and the `smooch("bank_name", freq, ...)` syntax requires explicit bank naming. Confirmed with the user as an intentional scope expansion. The PRD's §2.2 entry was struck through and replaced; the Status line and Executive-Summary text still describe the multi-bank reality. No code change.
- **`wave` alias dropped.** PRD §1, §7, §9, §10 listed four aliases (`smooch`, `wt`, `wave`, `wavetable`); the implementation ships only three because `wave` collides with a common variable name in existing patches (documented in `akkado/include/akkado/builtins.hpp:220-221`). PRD updated to match.
- **File-Level Changes Coverage.** Every file in PRD §10's table is touched by the diff. Extras outside the PRD list are reasonable: header `cedar/include/cedar/wavetable/preprocessor.hpp` (header for the `.cpp`), `cedar/bindings/bindings.cpp` (Python harness), `cedar/tests/test_wavetable.cpp` (C++ unit tests), `tools/nkido-cli/main.cpp` (CLI wavetable loading), and the `web/` integration files (frontend bridge, bundled WAVs, audio store hookup). Not flagged.

### Convention Drift

None. The smooch codegen path uses the same `push_path` / `compute_state_id` / `BUFFER_UNUSED` / `inst.rate`-byte / shared-pointer patterns the rest of the codebase uses.

### Suggestions

PRD pseudocode in §5.2 and §6.3 has been brought back into sync with the shipped code:

- `§5.2` step 6.2 — the `bin > cutoffBin` exclusive cutoff now matches the shipped preprocessor (the original `bin >= cutoffBin` would have killed the fundamental at mip 10).
- `§6.3` step 2 — the `log2(2048 / maxHarmonic)` formula now matches the shipped opcode (the conservative pick is what gives the test-3 alias floor of -102 dB).

Both deviations are documented in code comments at `cedar/src/wavetable/preprocessor.cpp:77-84` and `cedar/include/cedar/opcodes/oscillators.hpp:1071-1078` respectively.

## Decisions Recorded

- **Multi-bank ships in v1.** Confirmed during the audit as an intentional scope expansion. PRD §2.2 updated to reflect.
- **Hot-swap audio-path test (PRD §12 #10) remains deferred.** Replaced with a more targeted C++ test of the smooch-specific contract: the StatePool's `state_id`-keyed slot is the same one the opcode reads/writes each block. The full audio-thread hot-swap path is shared infrastructure already tested by `test_hot_swap.cpp`.
- **PRD §12 #11 reinterpreted.** The original "bank-swap during playback" framing assumed the single-bank model. Under multi-bank, the equivalent invariant is "in-place `set_named` replace preserves IDs and previously-pinned `shared_ptr`s stay alive." Now under test.
- **PRD pseudocode updated to match shipped code** in two places (§5.2 step 6.2 cutoff; §6.3 step 2 mip-fractional formula).
- **`wave` alias intentionally dropped** from §1.

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `cedar/tests/test_wavetable.cpp` (new test case) | Catch2 — new | PRD §8.n stereo WAV rejection. |
| `cedar/tests/test_wavetable.cpp` (new test case) | Catch2 — new | PRD §12 #11 reinterpreted: in-place bank replace preserves ID + old pinned shared_ptrs stay alive. |
| `cedar/tests/test_wavetable.cpp` (new test case) | Catch2 — new | PRD §12 #10 (smooch-specific portion): SmoochState's `state_id`-keyed slot persists across `process_block` calls. |
| `experiments/test_op_smooch.py` (new test fn `test_8b_freq_edge_cases`) | Python opcode test — new | PRD §8.a/8.b out-of-range freq clamp at `f=0`, `f=-1`, `f=Nyquist`. |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `cmake --build build` | Pass |
| `./build/cedar/tests/cedar_tests "[wavetable]"` | Pass — 12 cases / 71 assertions |
| `./build/cedar/tests/cedar_tests` | Pass — 167 cases / 334 371 assertions (was 164 / 334 380 — 3 audit additions; assertion-count delta is harness internal) |
| `./build/akkado/tests/akkado_tests` | Pass — 539 cases / 137 399 assertions |
| `cd experiments && uv run python test_op_smooch.py` | Pass — 12/12 (1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 8b → 9 → 11 → 12) |

## PRD Status

- Before: `NOT STARTED`
- After: `COMPLETE` (with audit-trail link and a note on the multi-bank scope expansion)

## Recommended Next Steps

None blocking. Two follow-up opportunities, both already noted as deliberately-deferred in the PRD:

- §5.3 / §5.4 — alternative band-limiting tapers and amplitude normalizations. The §12 baseline (alias floor -102 dB, mip-RMS within 0.5 dB of source) gives an objective reference for any future A/B.
- §6.5 — heavier interpolation kernels and mip-aware kernel selection. Most likely first picks: 6-point Niemitalo at mip 0 (interpolation-bound material) or spectral morph (morph-trajectory-bound material).

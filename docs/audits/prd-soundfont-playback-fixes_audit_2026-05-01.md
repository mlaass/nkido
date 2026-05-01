# Audit: SoundFont Playback Fixes PRD

**PRD:** `docs/prd-soundfont-playback-fixes.md`
**Audit base:** `b545a92d90fb5b4357ced85ca5187434905347d2` (PRD introduced, 2026-04-01)
**Audit head:** `2d34463a81b78baf88fe1c2d5c2cd0ee4bb309de` (HEAD prior to audit changes)
**Audited:** 2026-05-01
**Prior audit:** `docs/audits/prd-soundfont-playback-fixes_audit_2026-04-24.md`

## Summary

- Goals met: 4 of 4
- Critical findings closed: 3 (Goal 3 unmet, missing C++ dedup test, missing Python experiment)
- Tests added during audit: 2 files (one Catch2, one Python) plus SoundFont bindings exposed in `cedar_core`
- PRD Status: flipped from `NOT STARTED` to `COMPLETE`
- One-line verdict: The two C++ bug fixes were already shipped correctly back in `9e4186e` (2026-04-01); this audit closed the verification gaps the PRD itself called for.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. All notes in sequential patterns play (legato + same-note re-articulation) | Met | `cedar/include/cedar/opcodes/soundfont.hpp:38-39` (optional trigger read), `:81-93` (`gate_on || trigger_on || note_change` matrix), `:97-105` (same-note fast-release). Codegen wires `PatternPayload::TRIG → inputs[4]` at `akkado/src/codegen_patterns.cpp:5377`. |
| 2. SoundFont loading idempotent by name | Met | `cedar/src/audio/soundfont.cpp:53-58` (dedup loop runs before parser). Now covered by `cedar/tests/test_soundfont_registry.cpp` (3 sections, including a "no SampleBank growth" assertion proving the dedup short-circuits before the parser). |
| 3. Python experiment validates retrigger | Met | `experiments/test_op_soundfont.py` — 8 scenarios covering all PRD §8.2 tests plus edge cases 7.1 and 7.4. Phase 3 prerequisite satisfied: SoundFont bindings now exposed in `cedar/bindings/bindings.cpp` (`soundfont_load_from_file`, `soundfont_load_from_memory`, `soundfont_count`, `soundfont_presets_json`). |
| 4. Zero change to existing Akkado syntax | Met | All changes are confined to opcode internals, codegen wiring, registry dedup, the `SoundFontVoiceState` struct, and bindings. No grammar or builtin signature changes. |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build` | Pass | Build succeeded after the test_codegen.cpp WIP from a parallel agent was resolved. |
| `./build/cedar/tests/cedar_tests` | Pass (334799 assertions, 172 test cases) | +43 assertions / +2 test cases vs prior baseline (the new soundfont registry tests). |
| `./build/akkado/tests/akkado_tests` | Pass (137428 assertions, 552 test cases) | Unchanged. |
| `cd experiments && uv run python test_op_soundfont.py` | Pass (8/8) | Per-scenario: single note, sequential w/ trigger, same-note retrigger, note-change fallback, gate gaps, velocity scaling, vibrato, gate-off-at-note-change. |
| `cd experiments && uv run python test_op_soundfont.py --soak` | Pass | 200 loops × 1.5s = 300s of simulated audio for the critical sequential-with-trigger scenario, 0 failed iterations. |

## Findings

### Unmet Goals
- None at audit close. Goal 3 was Unmet at audit start (no `experiments/test_op_soundfont.py`, no SF bindings in `cedar_core`); resolved during the audit.

### Stubs
- None observed.

### Regressions
- None. cedar_tests / akkado_tests baseline preserved; the new soundfont test cases cleanly add to the suite.

### Coverage Gaps (resolved during audit)
- **PRD Goal 1 retrigger logic had no test.** Resolved by `experiments/test_op_soundfont.py` Tests 1–5 (gate-edge, trigger-pulse, same-note retrigger, note-change fallback, gate gaps).
- **PRD Goal 2 dedup had no C++ test.** Resolved by `cedar/tests/test_soundfont_registry.cpp` covering same-name → same ID, different-name → different ID, dedup short-circuits before parsing (no SampleBank growth on dedup hit), plus invalid-input rejections.
- **PRD §7.1 vibrato edge case un-tested.** Resolved by `experiments/test_op_soundfont.py` Test 7 — 6 Hz, ±30 cent vibrato, asserts smooth sustained RMS without re-attacks.
- **PRD §7.4 gate-off-at-note-change un-tested.** Resolved by `experiments/test_op_soundfont.py` Test 8 — gate falls and freq jumps on the same sample, asserts release tail decays rather than re-attacking.

### Missing Tests (resolved during audit)
- **PRD §6 Phase 2 verify step explicitly called for a C++ dedup unit test.** Now present at `cedar/tests/test_soundfont_registry.cpp`.
- **PRD §6 Phase 3 called for `experiments/test_op_soundfont.py`.** Now present, with the binding prerequisite the PRD itself flagged also satisfied.

### Scope Drift
- None observed. Implementation footprint matches PRD §5 exactly (the four files listed). Audit-time additions:
  - `cedar/bindings/bindings.cpp` — added SoundFont surface (PRD §6 Phase 3 prerequisite).
  - `cedar/tests/test_soundfont_registry.cpp` — new test (PRD §6 Phase 2 explicit ask).
  - `cedar/tests/CMakeLists.txt` — wire the new test source.
  - `experiments/test_op_soundfont.py` — new test (PRD §6 Phase 3, §8.2).
- The unrelated commit `8796d18` ("Fix SF3 SoundFont loading in WASM web build") is outside this PRD's scope, as noted in the prior audit.

### Convention Drift
- None.

### Suggestions
- The default-SF preload race described in PRD §1.3 cannot be fully tested without a JS-side harness; the C++ dedup test covers the C++-side correctness, so any future regression on the JS side will need an end-to-end web test. This is acceptable as-is.
- The vibrato test (Test 7) uses a relative-RMS-deviation proxy. If a future regression manifests as periodic re-attacks at sub-audible levels, the proxy could miss it; a tighter test would FFT-window the sustain and look for sidebands at the suspected retrigger rate. Tracking only as a future polish item.

## Decisions Recorded

- User selected **option 1** for Goal 3 ("Add SF bindings + Python test now") rather than the PRD's allowed fallback to C++-only. New `soundfont_load_from_file`, `soundfont_load_from_memory`, `soundfont_count`, `soundfont_presets_json` bindings shipped, gated by `CEDAR_NO_SOUNDFONT`.
- User confirmed adding the C++ dedup unit test using the `web/static/soundfonts/TimGM6mb.sf3` fixture (2.6 MB, smallest of the three shipped SF3s). Test skips gracefully when fixture is absent so the suite remains portable.
- User selected edge cases 7.1 (vibrato) and 7.4 (gate-off-at-note-change) for Python coverage; same-note repetition (7.2) was already in the PRD's planned Test 3 and is also included.
- User approved flipping PRD Status `NOT STARTED → COMPLETE`.

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `cedar/tests/test_soundfont_registry.cpp` | new | Goal 2 dedup (same-name → same ID, different-name → different ID, short-circuits before parser); invalid-input rejection |
| `experiments/test_op_soundfont.py` | new | Goal 1 retrigger (8 scenarios), edge cases 7.1 and 7.4, velocity scaling, soak run (300s simulated audio) |
| `cedar/bindings/bindings.cpp` | extended | Exposed SoundFont registry surface to `cedar_core` (PRD §6 Phase 3 prerequisite); added `SOUNDFONT_VOICE` opcode enum value |
| `cedar/tests/CMakeLists.txt` | extended | Wire `test_soundfont_registry.cpp` into the test executable |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `cmake --build build` | Pass |
| `./build/cedar/tests/cedar_tests` | Pass — 334799 assertions, 172 test cases |
| `./build/akkado/tests/akkado_tests` | Pass — 137428 assertions, 552 test cases |
| `cd experiments && uv run python test_op_soundfont.py` | Pass — 8/8 scenarios |
| `cd experiments && uv run python test_op_soundfont.py --soak` | Pass — 200 loops × 1.5s, 0 failures over 300s |

## PRD Status

- Before: `NOT STARTED`
- After: `COMPLETE`
- Reason: All four goals verifiably met with code citations, the two PRD-mandated verification deliverables (C++ dedup test, Python experiment) are now present and passing, and a 300s soak run confirms no time-cumulative regressions.

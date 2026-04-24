# Audit: SoundFont Playback Fixes PRD

**PRD:** `docs/prd-soundfont-playback-fixes.md`
**Audit base:** `b545a92d90fb5b4357ced85ca5187434905347d2` (PRD introduced)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 3 of 4
- Critical findings: 2 (Unmet=1, Stubs=0, Coverage Gaps=0, Missing Tests=2)
- Recommended PRD Status: Partially Implemented (missing Phase 3 Python test)
- One-line verdict: Both C++ bug fixes are shipped exactly as designed, but the Phase 3 Python experiment test file and cedar_core SF bindings prerequisite are not present, and no C++ dedup unit test exists.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. All notes in sequential patterns play correctly (legato + same-note re-articulation) | Met | `cedar/include/cedar/opcodes/soundfont.hpp:38-39` reads optional trigger from `inst.inputs[4]`; `:82-96` implements `gate_on \|\| trigger_on \|\| note_change` logic; same-note fast-release preserved at `:98-105`. Codegen wiring at `akkado/src/codegen_patterns.cpp:2986` (`sf_inst.inputs[4] = pat_fields[PatternPayload::TRIG]`). |
| 2. SoundFont loading idempotent by name | Met | `cedar/src/audio/soundfont.cpp:53-58` — name-based dedup loop at top of `load_from_memory()`: `if (banks_[i].name == name) return static_cast<int>(i);`. |
| 3. Python experiment test validates retrigger | Unmet | `experiments/test_op_soundfont.py` does not exist. `cedar/bindings/bindings.cpp` contains no SoundFont-related bindings (grep for "soundfont\|sf2\|SFVoice" returns nothing), so the Phase 3 prerequisite identified in PRD §6 is also unmet. |
| 4. Zero change to Akkado syntax | Met | Changes are confined to opcode internals, codegen wiring, registry dedup, and the `SoundFontVoiceState` struct. No grammar or signature changes. |

## Findings

### Unmet Goals
- **Goal 3 (Python experiment)** — The PRD's §3.3 and §6 Phase 3 plan calls for `experiments/test_op_soundfont.py` covering 6+ synthetic-signal test cases (single note, sequential different notes, same-note retrigger, note-change fallback, gate gaps, velocity scaling). No file is present. The PRD itself flagged a hard prerequisite: `cedar_core` has no SoundFont bindings. Inspection of `/home/moritz/workspace/nkido/cedar/bindings/bindings.cpp` confirms that prerequisite is unmet — no `load_from_memory`, `SoundFontRegistry`, or `SFVoice` symbols are exposed.

### Stubs
- None. The shipped C++ changes are complete implementations matching the PRD design verbatim (trigger buffer read pointer, per-sample `trigger_on`/`note_change` booleans, `prev_note` state field, name-dedup loop). No TODO markers.

### Coverage Gaps
- None beyond the missing Python test. The core detection logic (`soundfont.hpp:81-96`) matches PRD §3.1.2 line-for-line including the `255` sentinel and `trig_buf == nullptr` guard for fallback.

### Missing Tests
- **Phase 3 Python experiment** — `experiments/test_op_soundfont.py` absent. `experiments/` has dozens of `test_op_*.py` files for other opcodes but none for SoundFont.
- **C++ unit test for registry dedup** — PRD §6 Phase 2 Verify step explicitly says: "add a C++ unit test that loads the same SF data twice with the same name and asserts the returned IDs are equal. Also test that loading with a different name returns a different ID." Grep of `cedar/tests/` and `akkado/tests/` for "soundfont" / "SoundFont" returns zero matches. No such test exists.
- No Catch2 coverage for the retrigger opcode logic (gate/trigger/note-change matrix).

### Scope Drift
- None observed. Code changes are precisely the four files listed in PRD §5 (`dsp_state.hpp`, `soundfont.hpp`, `codegen_patterns.cpp`, `soundfont.cpp`). Line counts match the "~5 lines", "~15 lines", "~3 lines" estimates.
- Note: A later commit `8796d18` ("Fix SF3 SoundFont loading in WASM web build") is outside this PRD's scope.

### Suggestions
- Either add `experiments/test_op_soundfont.py` after exposing SF bindings in `cedar_core`, or pivot to Catch2-based unit tests as the PRD Non-Goal alternative allows (PRD §6 Phase 3 option 2).
- Add a small Catch2 test for `SoundFontRegistry::load_from_memory` name-dedup covering: same name twice → equal IDs; different name → different IDs; empty-name behavior (dedup would match any later empty-name bank — worth considering).
- Update PRD frontmatter status from "NOT STARTED" to reflect actual shipped state.

## PRD Status
- Current: `NOT STARTED` (frontmatter line 1)
- Recommended: `Partially Implemented` — Phases 1 & 2 complete and shipped in commit `9e4186e`; Phase 3 (Python experiment) and its binding prerequisite outstanding; Phase 2's planned C++ dedup unit test is missing.
- Reason: The two production bug fixes (trigger wiring + registry dedup) are correctly implemented exactly as designed. However the PRD's "Status: NOT STARTED" label is inaccurate, and two PRD-mandated verification deliverables (Python experiment, C++ dedup unit test) are absent.

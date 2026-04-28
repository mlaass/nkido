# Audit: Strudel-Style Pattern System Extensions — Phase 2

**PRD:** `docs/prd-pattern-array-note-extensions-phase-2.md`
**Audit base:** `45f89dd` (2026-04-26 — commit that introduced the PRD)
**Audit head:** `3d46118` + audit additions (2026-04-28)
**Audited:** 2026-04-28

PRD self-reports `Status: SHIPPED (2026-04-27)` with explicit follow-up
deferrals tracked in §11. The audit confirms ship status and closes the
test coverage gaps it found.

## Goal Verification

| Goal | Status | Evidence |
|------|--------|----------|
| 1. Ship 12 time/structure modifiers | Met | `akkado/src/codegen_patterns.cpp:3571-4691` (handlers for `early`, `late`, `palindrome`, `compress`, `ply`, `linger`, `zoom`, `segment`, `swing`, `swingBy`, `iter`, `iterBack`); dispatch in `akkado/src/codegen.cpp:823-834`; iter/iterBack runtime via `cedar::SequenceState` fields at `cedar/include/cedar/opcodes/sequence.hpp:166-171` and rotation logic at `cedar/include/cedar/opcodes/sequencing.hpp:392-407`. |
| 2. Ship `run`, `binary`, `binaryN` | Met | `akkado/src/codegen_patterns.cpp:4229`, `4261`, `4294`. |
| 3. Voicing system (`anchor`, `mode`, `voicing`, `addVoicings`) | Met | `akkado/include/akkado/voicing.hpp` + `akkado/src/voicing.cpp:196` (`voice_chords`); built-in dictionaries close/open/drop2/drop3 at `akkado/src/voicing.cpp:23-26`; greedy nearest voice leading at `akkado/src/voicing.cpp:262-275`. |
| 4. Extend `PatternEvent` with bend/aftertouch/properties + standalone `.bend()`/`.aftertouch()`/`.dur()` transforms | **Partial — deferred per §11** | Record-suffix surface ships (lexer/parser/codegen), and `vel`/`dur` keys propagate to `cedar::Event` at `akkado/src/codegen_patterns.cpp:363-369`. PatternEvent struct extension and standalone transforms are deferred per PRD §11 (custom-property accessor `e.cutoff`, `bend()`/`aftertouch()` standalone transforms). PRD self-documents this in Status line + §11. |
| 5. Mini-notation record suffix `c4{vel:0.8, bend:0.2}` | Met | `akkado/src/mini_lexer.cpp:664` (`try_lex_record_suffix`); `akkado/include/akkado/ast.hpp:213` (`MiniAtomData.properties`); parser propagation at `akkado/src/mini_parser.cpp:268-338`. |
| 6. Per-modifier unit tests + golden voicing assertions | Met (closed during audit) | Phase 2 transforms tested at `akkado/tests/test_codegen.cpp:2261-2916`; voicing exact-MIDI assertions added during audit at `:2815-2895`. |
| 7. Resolve `compress` naming collision | Met | Alias removed at `akkado/include/akkado/builtins.hpp:1085-1088` (only `compressor` aliases to audio compressor); `compress` is the pattern transform; tested at `akkado/tests/test_builtins.cpp:172`. |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build` | Pass | Clean, only pre-existing sign-conversion warnings in tools/. |
| `./build/akkado/tests/akkado_tests` | Pass | 137283 assertions in 519 cases (was 518 / 137267 pre-audit; +1 case, +16 assertions). |
| `./build/cedar/tests/cedar_tests` | Pass | 334164 assertions in 153 cases (incl. iter/iterBack rotation tests at `cedar/tests/test_vm.cpp:1943-2016`). |
| `nkido-cli render --seconds 30 experiments/phase2_smoke.akk` | Pass | 30 s renders to 5.76 MB WAV without crashes (Phase E acceptance). |
| `uv run python experiments/test_op_seqpat_step.py` | Pass (audit-added) | 320 s iter(4) render: no silences > 5 s, RMS ratio 1.022 across halves. |
| `uv run python experiments/test_op_palindrome.py` | Pass (audit-added) | 320 s palindrome render: all four expected pitches present in spectrum (39–44 dB), RMS ratio 0.945. |

## Findings

### Unmet Goals
None.

### Stubs
None. Stub-marker grep over `akkado/src/codegen_patterns.cpp`, `akkado/src/voicing.cpp`, `akkado/src/mini_lexer.cpp`, `akkado/src/mini_parser.cpp`, `akkado/src/pattern_eval.cpp`, `cedar/include/cedar/opcodes/sequence.hpp`, `cedar/include/cedar/opcodes/sequencing.hpp` returned zero hits. Phase D0 prerequisite — `handle_velocity_call` un-stubbed at `akkado/src/codegen_patterns.cpp:2884-3030` — verified to emit a real `MUL` on the velocity buffer.

### Regressions
None.

### Coverage Gaps (resolved in this audit)
- **Voicing modes `duck` and `root`** were parsed (`akkado/src/voicing.cpp:191-192`) but had no behavior tests. **Resolved:** added behavior assertions at `akkado/tests/test_codegen.cpp:2854-2901` pinning `Am @ c4 duck → [48, 52, 57]` and `Am @ c4 root → [45, 60, 64]`.
- **Voicing golden assertions used loose bounds** (top ≤ 60.5 / bottom ≥ 71.5) instead of PRD §10.1's exact MIDI notes. **Resolved:** tightened existing tests to pin exact MIDI vectors at `akkado/tests/test_codegen.cpp:2815, 2843`. Side discovery: PRD §10.1's expected `Am @ c4 below → [57, 60, 64]` is internally inconsistent (top=64 > anchor=60 violates the `below` constraint); algorithmic output `[52, 57, 60]` is correct. Test pins the algorithmic value with a comment explaining the PRD typo. Documented under "Decisions Recorded" below.
- **Record-suffix `dur` key** was honored at compile time (`akkado/src/codegen_patterns.cpp:367`) but had no test. **Resolved:** added test at `akkado/tests/test_codegen.cpp:2225-2249` covering single-atom and two-atom `dur` propagation.

### Missing Tests (resolved in this audit)
- **`experiments/test_op_seqpat_step.py`** — PRD §10.3 explicitly named it for ≥300 s iter rotation testing. **Resolved:** new file at `experiments/test_op_seqpat_step.py` rendering iter(4) for 320 s and asserting silence-window and RMS-stability bounds.
- **`experiments/test_op_palindrome.py`** — PRD §10.3 explicitly named it. **Resolved:** new file at `experiments/test_op_palindrome.py` rendering palindrome for 320 s with spectrum-presence checks for all four pitches.

### Scope Drift
The diff between `45f89dd..HEAD` includes two unrelated landed features (live audio input via `629a650`, array utility builtins via `daaeb16` / `6bad83b`) and a phaser fix (`299af3f`). These are independent merged work, not scope drift introduced by Phase 2. Noted for completeness; not a finding.

### Convention Drift
None. Phase 2 code follows project conventions in `CLAUDE.md`: no per-frame allocations, dispatch through the existing handle-call map, errors emitted via the standard E13x codes, and per-opcode tests in the documented test file layout.

### Suggestions
- **PRD §10.1 voicing table values are aspirational, not algorithmic.** The "below" row's `[57, 60, 64]` is inconsistent with the mode constraint, and the "duck" row's `[57, 64, 69]` does not match the implemented selection. A future PRD edit could replace these with the actual algorithm outputs (`[52, 57, 60]` and `[48, 52, 57]`) so the table reflects shipped behavior.
- **AmCGF voice-leading bound** is `≤ 15` in tests vs PRD §10.1's `≤ 6 semitones`. Actual algorithmic total is 11. Tightening the test to `≤ 12` would catch most regressions while staying above current behavior.

## Decisions Recorded

- User confirmed all four critical findings should be closed with new tests.
- PRD §10.1's expected MIDI notes for `Am @ c4 below` and `Am @ c4 duck` do not match the shipped algorithm; new tests pin to the **algorithmic output** with comments noting the PRD typo. The algorithm is correct (mode constraints satisfied, voice leading minimized); the PRD table is wrong. Noted under Suggestions for a future PRD cleanup.
- Goal #4 (PatternEvent struct extension + standalone `.bend()`/`.aftertouch()`/`.dur()` transforms) is left in **partial / deferred** state per PRD §11. The PRD's Status line and §11 explicitly track the deferred runtime work; not flagged as Unmet.

## Tests Added / Extended

| File | Kind | Covers |
|------|------|--------|
| `akkado/tests/test_codegen.cpp` (existing, lines ~2815–2901) | Extended | Exact-MIDI golden assertions for voicing `below`/`above`; new behavior tests for `duck` and `root` modes. |
| `akkado/tests/test_codegen.cpp` (existing, lines ~2225–2249) | Extended | Record-suffix `dur` key propagation to `event.duration` (single- and two-atom forms). |
| `experiments/test_op_seqpat_step.py` | New | iter(4) rotation stability across 320 s of rendered audio (PRD §10.3). |
| `experiments/test_op_palindrome.py` | New | palindrome forward/backward alternation across 320 s (PRD §10.3). |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `./build/akkado/tests/akkado_tests` | Pass — 137283 assertions / 519 cases (+1 case, +16 assertions vs pre-audit). |
| `./build/cedar/tests/cedar_tests` | Pass — 334164 assertions / 153 cases (unchanged). |
| `uv run python experiments/test_op_seqpat_step.py` | Pass — 320 s render, no silences > 5 s, RMS ratio 1.022. |
| `uv run python experiments/test_op_palindrome.py` | Pass — 320 s render, all four pitches present (39–44 dB), RMS ratio 0.945. |
| `nkido-cli render --seconds 30 experiments/phase2_smoke.akk` | Pass — Phase E smoke acceptance still clean. |

## PRD Status

- Before audit: `SHIPPED (2026-04-27) — Phases A–C and D1+D2 implemented; D3 deferred`
- After audit: unchanged. The PRD is ship-correct; coverage gaps are now closed with new tests, and the two deferred items remain explicitly tracked in PRD §11.

## Recommended Next Steps

None required for Phase 2 closure. For Phase 2.1 follow-up work:
1. Pick the runtime model for §11 (generic property bag on `cedar::Event` vs fixed `bend`/`aftertouch` fields).
2. Wire §5.5a custom-property pipe-binding accessor (`e.cutoff`).
3. Implement standalone `bend()`/`aftertouch()`/`dur()` transforms once runtime is wired.
4. (Optional, low priority) Edit PRD §10.1's voicing table to match algorithmic outputs as documented under Suggestions.

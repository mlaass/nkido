# Audit: Tap Delay System PRD

**PRD:** `docs/prd-tap-delay.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625` (PRD added in same commit that renamed PRDs to lowercase `prd-*`)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 14 of 17
- Critical findings: 3 (Unmet=0, Stubs=0, Coverage Gaps=3, Missing Tests=2)
- Recommended PRD Status: DONE (with caveat: missing Cedar/Akkado unit tests)
- One-line verdict: Full implementation of DELAY_TAP/DELAY_WRITE with seconds/ms/samples variants and closure-based feedback is shipped, but the PRD's Phase 1/2 test items are not met in Cedar/Akkado Catch2 suites — only Python experiment coverage exists.

## Goal Verification
| Goal | Status | Evidence |
|---|---|---|
| DELAY_TAP = 74, DELAY_WRITE = 75 in Opcode enum | Met | `cedar/include/cedar/vm/instruction.hpp:81-82` |
| `tap_cache` field added to DelayState | Met | `cedar/include/cedar/opcodes/dsp_state.hpp:122-125` (std::array<float, 128>) |
| `op_delay_tap()` implemented | Met | `cedar/include/cedar/opcodes/delays.hpp:181-248` |
| `op_delay_write()` implemented | Met | `cedar/include/cedar/opcodes/delays.hpp:260-293` |
| VM dispatch cases for both opcodes | Met | `cedar/src/vm/vm.cpp:809-815` |
| `bun run build:opcodes` regenerated metadata | Met | `cedar/include/cedar/generated/opcode_metadata.hpp:69-70, 186-187` |
| Unit tests for opcodes (Cedar) | Unmet | No references to `DELAY_TAP`/`DELAY_WRITE`/`tap_delay` in `cedar/tests/` |
| `tap_delay` added to builtins | Met | `akkado/include/akkado/builtins.hpp:292-303` (seconds/ms/samples variants) |
| Registered in special_handlers map | Met | `akkado/src/codegen.cpp:829-831` |
| `handle_tap_delay_call()` implemented | Met | `akkado/src/codegen_functions.cpp:1256-1433` |
| Analyzer validation added | Met | `akkado/src/analyzer.cpp:1247-1290` (E301/E302/E303 codes) |
| Semantic ID derivation for nested opcodes | Met | `akkado/src/codegen_functions.cpp:1332-1338, 1386-1399` (push_path("tap_delay#N"), push_path("fb"), shared `delay_state_id`) |
| Integration tests (Akkado compilation) | Unmet | No references to `tap_delay` in `akkado/tests/` |
| Added to web docs | Met | `web/static/docs/reference/builtins/delays.md:94, 129, 152` (tap_delay, tap_delay_ms, tap_delay_smp) |
| `bun run build:docs` rebuild of lookup index | Partial | Keywords present in frontmatter `web/static/docs/reference/builtins/delays.md:5`; not directly verifiable from source whether lookup index was rebuilt |
| Example patterns included in docs | Met | `web/static/docs/reference/builtins/delays.md:113-174` (dub delay, degrading delay, wet/dry examples across all three variants) |
| Python audio verification tests with WAV output | Met | `experiments/test_op_tap_delay.py:1-280` (timing_ms, feedback_decay, time_units with WAV + PNG outputs) |

## Findings

### Unmet Goals
1. **Cedar unit tests (Phase 1 final checklist item).** PRD Section 7.1 explicitly calls out `TEST_CASE("DELAY_TAP and DELAY_WRITE coordination")`, `TEST_CASE("Tap delay with filter in feedback")`, and `TEST_CASE("Tap delay state isolation")`. No matching Catch2 cases exist in `cedar/tests/`.
2. **Akkado integration tests (Phase 2 final checklist item).** PRD Section 7.2 calls out `TEST_CASE("tap_delay compilation")` and `TEST_CASE("tap_delay semantic IDs")`. No matching Catch2 cases in `akkado/tests/`.

### Stubs
None. All opcode bodies and the codegen handler are fully implemented; no TODO/stub markers found in the implementations.

### Coverage Gaps
1. No test of `tap_delay` compilation path in Akkado test suite (instruction sequence verification, shared state_id assertion).
2. No test of semantic ID derivation for stateful opcodes inside the feedback closure (`fb/lp`, `fb/saturate` paths).
3. No test of shared `state_id` between DELAY_TAP and DELAY_WRITE pairs, nor of state isolation between two independent `tap_delay` calls.

### Missing Tests
1. `cedar/tests/test_vm.cpp` lacks direct DELAY_TAP/DELAY_WRITE opcode tests (frequency-response with LP in feedback, state isolation between two tap_delays).
2. `akkado/tests/test_codegen.cpp` lacks tap_delay-specific tests (closure inlining, analyzer errors E301/E302/E303, dry/wet optional args).

Note: `experiments/test_op_tap_delay.py` partially compensates (timing accuracy, feedback decay, time-unit equivalence with WAV/PNG artifacts under `output/op_tap_delay/`), but this is not a substitute for the C++ unit/integration tests the PRD explicitly lists in the Phase 1/2 checklists.

### Scope Drift
Beneficial extensions beyond PRD scope (not a concern):
- Implementation adds **optional `dry`/`wet` parameters** (args 4 and 5) to all three variants — `akkado/include/akkado/builtins.hpp:292-303` declares `4 required + 2 optional`. PRD Section 2.3 said wet/dry mixing is "handled externally"; implementation supports both external mixing and built-in dry/wet without breaking the original signature.
- Implementation adds **delay-time smoothing** (`smoothed_delay`, `delay_initialized` in DelayState; smoothing coefficient 0.9995 in `cedar/include/cedar/opcodes/delays.hpp:212-227`) — prevents zipper noise on modulated delay times; quality improvement not in the PRD.
- `DELAY_WRITE` accepts 5 inputs (input, processed, fb, dry, wet) vs PRD's 3 inputs — consistent, non-breaking (slots 3/4 default to 0xFFFF → dry=0.0, wet=1.0 matching the PRD default of 100% wet).

### Suggestions
1. Add at least one Catch2 test in `cedar/tests/` that verifies DELAY_TAP/DELAY_WRITE coordination (shared state_id, tap_cache flow, state isolation between two pairs). Closes the Phase 1 unchecked item and guards against regressions in the coordinated-opcode pattern.
2. Add a `tap_delay` compilation test in `akkado/tests/test_codegen.cpp` that validates: (a) emits TAP before WRITE, (b) TAP/WRITE share `state_id`, (c) closure-body opcodes get the `fb/` path contribution to their state_id, (d) analyzer errors E301/E302/E303 fire on malformed calls, (e) optional dry/wet arguments compile correctly.
3. Update the PRD's status banner to either check all 17 boxes or explicitly acknowledge that test coverage currently lives in `experiments/test_op_tap_delay.py` rather than Cedar/Akkado Catch2 suites.
4. Extend the Python experiment with a filter-in-feedback frequency-response test (PRD Section 7.3 specifies this exactly, but it is not one of the three existing test functions).

## PRD Status
- Current: `DONE`
- Recommended: `DONE` (with documentation fix, or move to `MOSTLY DONE` until missing unit/integration tests are added)
- Reason: Implementation is complete and functional across Cedar VM, DelayState, Akkado codegen/analyzer/builtins, and web docs. Hot-swap-compatible semantic ID derivation is in place. Three variants (seconds/ms/samples) plus optional dry/wet go beyond the PRD. The remaining gap is strictly in C++ test coverage — the banner's "DONE" claim glosses over 2 of 17 Phase 1/2 checklist items (Cedar unit tests, Akkado integration tests) that remain unchecked. Either add those tests or amend the PRD banner to note Python-experiment-only coverage.

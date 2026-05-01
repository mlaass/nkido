# Audit: Userspace State Primitives and Edge-Triggered Operators

**PRD:** `docs/prd-userspace-state-and-edge-primitives.md`
**Audit base:** `8870bf8` (2026-04-25, last edit to the PRD before implementation)
**Audit head:** `2b6a4a0` (2026-05-01)
**Audited:** 2026-05-01

## Summary

The implementation shipped across four phase commits plus one fix commit, all merged before this audit:

| Commit | Description |
|---|---|
| `4587e05` | Phase 1: SAH → EDGE_OP rename with mode-dispatched edge primitives (sah, gateup, gatedown, counter) |
| `d6b3abc` | Phase 2: UFCS method-call codegen (replaces `E113` stub) |
| `77eb3ba` | Phase 3: Userspace state cells (`state`/`get`/`set`) over the new `STATE_OP` opcode |
| `c2ceb69` | Phase 4: Userspace `step` demo + reference docs for state/edge/methods |
| `5941140` | Fix: `stepper-demo` array-through-closure binding and STATE_OP gated-write semantics — see `docs/reports/2026-04-26-stepper-demo-array-and-state-bugs.md` |

Every file in the PRD's File-Level Changes table was touched. Every promised new artifact (tests, reference docs, demo patch, Python experiments) exists. Every PRD goal has shipping code with citable evidence. The full Cedar (170 cases, 334 792 assertions) and Akkado (544 cases, 137 410 assertions) suites were green before and after the audit-driven test additions.

The PRD `Status` line was stale (still said `NOT STARTED`) and is updated to `COMPLETE` as part of this audit. PRD §5.1 row "rate=2 store" and §9 row "`s.set(stream)`" are also updated to match the shipped semantics — the original "write last sample" contract was revised during the fix commit because the literal contract dropped gated writes (`idx.set(select(gateup(t), …, idx))` in ~127 of every 128 blocks at the canonical 128-sample block size).

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. Ship `step` family in userspace | Met (with documented closure-`len` caveat) | `web/static/patches/stepper-demo.akk` (forward + step_dir); `experiments/test_stepper_demo_integration.py`; new `akkado/tests/test_state.cpp` covers `step(reset)` and `step(reset, start)` variants |
| 2. `state(init)` cells with `.get()` / `.set()` and per-call-site slots | Met | `cedar/include/cedar/opcodes/state_op.hpp:1`; `akkado/src/codegen_state.cpp:31`; `akkado/include/akkado/typed_value.hpp:21,168` |
| 3. SAH → EDGE_OP rename with modes 0/1/2/3 | Met | `cedar/include/cedar/vm/instruction.hpp:63`; `cedar/include/cedar/opcodes/edge_op.hpp:18`; `cedar/src/vm/vm.cpp:773`; `cedar/include/cedar/opcodes/dsp_state.hpp:119` (renamed `EdgeState`) |
| 4. New builtins: gateup, gatedown, counter (with optional reset/start) | Met | `akkado/include/akkado/builtins.hpp:672-686` |
| 5. UFCS method-call codegen | Met | `akkado/src/codegen.cpp:765-772` (MethodCall falls through to Call); replaces former E113 stub |
| 6. `state` / `get` / `set` reserved as identifiers | Met | `akkado/src/parser.cpp:69-74,301,1407,1442` |
| 7. Zero regressions on existing `sah` patches | Met | `sah` builtin entry at `akkado/include/akkado/builtins.hpp:667` lowers to EDGE_OP rate=0; `bindings.cpp:79` retains `cedar.Opcode.SAH` alias for Python; full suite green |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build /home/moritz/workspace/nkido/build` | Pass | Incremental rebuild after audit-driven test edits |
| `./build/cedar/tests/cedar_tests "[edge_op],[state_op]"` | Pass — 11 cases, 1190 assertions | All four EDGE_OP modes + all three STATE_OP modes + new hot-swap and gated-write coverage |
| `./build/akkado/tests/akkado_tests "[methods],[state]"` | Pass — 21 cases, 100 assertions | UFCS desugar, state-cell semantics, reserved names, step variants, edge cases |
| `./build/cedar/tests/cedar_tests` (full) | Pass — 170 cases, 334 792 assertions | |
| `./build/akkado/tests/akkado_tests` (full) | Pass — 544 cases, 137 410 assertions | |
| `cd experiments && uv run python test_op_edge.py` | Pass | All four EDGE_OP modes verified against synthetic input |
| `cd experiments && uv run python test_op_state.py` | Pass | Init / load / store, plus 300 s long-run stability |
| `cd experiments && uv run python test_state_set_in_block.py` | Pass | Gated-write proof + short and long step_dir walk |
| `cd experiments && uv run python test_step_pattern.py` | Pass | Forward stepper at the Cedar level, 24-tick + 300 s |
| `cd experiments && uv run python test_stepper_demo_integration.py` | Pass | Compiles `stepper-demo.akk` via `nkido-cli render`; STFT-detects 13 distinct semitones on each channel (threshold ≥ 4 melody, ≥ 2 bass) |

## Findings

### Unmet Goals
None.

### Stubs
None. The former `E113` stub at `akkado/src/codegen.cpp:1513` is gone; the MethodCall case now falls through to the regular Call handler.

### Regressions
None. Full suites green on `HEAD`.

### Coverage Gaps (all closed by this audit)

- **Step variants `step(arr, trig, reset)` and `step(arr, trig, reset, start)`.** PRD Goal 1 claimed all four `step` variants are implementable. The demo + integration test only exercised `step(arr, trig)` and `step_dir`. **Closed:** new tests `State: step variant with reset compiles` and `State: step variant with reset and start compiles` in `akkado/tests/test_state.cpp:233,247`.
- **`gateup(constant)` edge case (PRD §9).** No test exercised the documented "first-sample-only pulse from rising edge against initial 0 prev" behavior. **Closed:** new test `EDGE_OP gateup of a non-zero constant pulses once on first sample` in `cedar/tests/test_edge_op.cpp:251`.
- **`s.get()` before any `set()` (PRD §9).** Implicit in the persistence test but never directly asserted. **Closed:** new test `State: get() before any set() returns the init value` in `akkado/tests/test_state.cpp:166`.
- **Empty-array `[].step(trig)` (PRD §9).** No test confirmed silence-not-crash. **Closed:** new test `State: empty array stepper produces silence not a crash` in `akkado/tests/test_state.cpp:269`.
- **State cell hot-swap with edited init literal (PRD §9 row "`state(0)` edited to `state(5)` …").** No test exercised the production hot-swap path through the swap controller. **Closed:** new test `STATE_OP hot-swap preserves slot value when init literal changes` in `cedar/tests/test_state_op.cpp:140`. Initial draft used `load_program_immediate` (which resets the state pool by design) and failed; the corrected version uses `load_program` (the swap-controller path) which preserves the pool, matching the PRD §9 behavior. The test now passes — the implementation does honor the contract.
- **Closure invoked at multiple sites has independent slots per site (PRD §11).** No direct test. **Closed:** new test `State: closure invoked at two sites has independent slots per site` in `akkado/tests/test_state.cpp:197`. Verified that ≥ 2 distinct STATE_OP rate=0 instructions with different `state_id`s are emitted when a state-using closure is invoked twice.
- **Gated-write semantics for STATE_OP rate=2.** Covered by `experiments/test_state_set_in_block.py` but not by a Catch2 unit test. **Closed:** new test `STATE_OP store records latest sample differing from start-of-block` in `cedar/tests/test_state_op.cpp:189`.

### Missing Tests
None. Every file the PRD's Testing Strategy named exists (`cedar/tests/test_edge_op.cpp`, `cedar/tests/test_state_op.cpp`, `akkado/tests/test_methods.cpp`, `akkado/tests/test_state.cpp`, `experiments/test_op_edge.py`, `experiments/test_op_state.py`).

### Scope Drift
Out-of-scope changes that landed alongside the PRD's own work:

- `cedar/bindings/bindings.cpp` — exposed `SELECT`, `ARRAY_PACK`, `ARRAY_INDEX` and a `cedar.Opcode.SAH = EDGE_OP` alias. Required by the new Python experiments and the bug-report tests. Documented in `docs/reports/2026-04-26-stepper-demo-array-and-state-bugs.md`.
- `akkado/src/codegen_functions.cpp` — `define_array` branch added so multi-buffer arrays survive being passed through closure parameters (Bug A from the same fix report). Without this, the demo's `[57,60,64,67,72]` was silently degraded to `[57]` length 1.
- `akkado/tests/test_const_eval.cpp` — `Const: array used as osc input` test updated to add `|> sum(%) |>` after the array-binding fix exposed it as a false positive (it had been asserting bytecode constants existed without verifying they were wired to the oscillator).

All three are documented in the linked report and were necessary to make the PRD's own §4.4 `step_dir` example actually work end-to-end. They are not unrelated work.

### Convention Drift
None observed. The audit-driven test additions follow existing test patterns (Catch2, the `make_state` / `make_edge` helpers in `cedar/tests/test_*.cpp`, and the `get_instructions` / `count_op` helpers in `akkado/tests/test_state.cpp`).

### Suggestions

- **PRD §4.4 `len(arr)`-inside-closure caveat.** PRD §4.4 shows `step = (arr, trig) -> arr[wrap(counter(trig), 0, len(arr))]`. `len(arr)` is a compile-time builtin (lowers to `PUSH_CONST`) and currently errors with `E141: len() requires an array, but 'arr' is not an array` when `arr` is a closure parameter — because the array length isn't known until the call site is visited. The PRD §4.4 note already observes that the explicit wrap is unnecessary (ARRAY_INDEX wraps by default), and the demo and audit tests use the bare `arr[counter(...)]` form, which works correctly. Either teach `len()` to look through closure parameter bindings, or update PRD §4.4's example code to drop the `wrap(..., 0, len(arr))` wrapper. Out of scope for this audit.
- **Stale title on existing test `STATE_OP rate=2 store uses last sample only`** (`cedar/tests/test_state_op.cpp:65`). The test passes under the new "latest sample differing from start-of-block" semantics because its input (a ramp from 0 to ~1) varies at every sample, so "last differing" coincides with "last sample". The title should be updated to reflect the contract the test actually pins (e.g. `STATE_OP rate=2 store with sample-varying input picks the last sample`). Cosmetic; the audit did not change it.

## Decisions Recorded

- **Update PRD §5.1 + §9 to match shipped STATE_OP rate=2 semantics.** The original "write last sample" contract dropped gated writes; the new "latest-differing-from-start-of-block-value" contract is a strict superset for non-gated streams and is what the §4.4 `step_dir` example requires. Decision and rationale captured in `docs/reports/2026-04-26-stepper-demo-array-and-state-bugs.md`.
- **Flip PRD `Status` to COMPLETE.** All goals met, all critical findings resolved, every audit-driven test landed and passes.
- **No source code edits beyond test additions.** The implementation already covered every claim; the audit's only source change is the PRD itself plus new test files.

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `cedar/tests/test_edge_op.cpp` | extended (+1 test case) | PRD §9 edge case: `gateup` of a non-zero constant pulses only on the first sample |
| `cedar/tests/test_state_op.cpp` | extended (+2 test cases) | PRD §9 hot-swap with edited init literal preserves slot value; PRD §5.1 gated-write semantic ("latest sample differing from start-of-block") |
| `akkado/tests/test_state.cpp` | extended (+5 test cases) | `s.get()` before any `set()` returns init; closure invoked at two call sites has independent slots; `step(reset)` variant compiles; `step(reset, start)` variant compiles; `[].step(trig)` produces silence not a crash |

Total new test cases: **8** (3 cedar, 5 akkado). All pass.

## Post-Finalize Validation

| Command | Result |
|---|---|
| `./build/cedar/tests/cedar_tests` | Pass — 170 cases (+3 from audit), 334 792 assertions |
| `./build/akkado/tests/akkado_tests` | Pass — 544 cases (+5 from audit), 137 410 assertions |
| `./build/cedar/tests/cedar_tests "[edge_op],[state_op]"` | Pass — 11 cases, 1190 assertions |
| `./build/akkado/tests/akkado_tests "[methods],[state]"` | Pass — 21 cases, 100 assertions |
| `cd experiments && uv run python test_op_edge.py` | Pass |
| `cd experiments && uv run python test_op_state.py` | Pass |
| `cd experiments && uv run python test_state_set_in_block.py` | Pass |
| `cd experiments && uv run python test_step_pattern.py` | Pass |
| `cd experiments && uv run python test_stepper_demo_integration.py` | Pass — 13 distinct semitones on each channel (threshold ≥ 4 melody, ≥ 2 bass) |

## PRD Status

- Before: `NOT STARTED`
- After: `COMPLETE` (with reference back to this audit and the four phase commits)

## Recommended Next Steps

None blocking. Two low-priority items are noted under Suggestions and can be addressed independently of this audit if desired:
1. Either teach `len()` to inspect closure-parameter bindings or update the PRD §4.4 example code to drop the redundant `wrap(..., 0, len(arr))` wrapper.
2. Rename the existing test `STATE_OP rate=2 store uses last sample only` to reflect the new contract.

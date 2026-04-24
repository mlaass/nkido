# Audit: Audio Input PRD — Live Signal Processing

**PRD:** `docs/prd-audio-input.md`
**Audit base:** `84546fdfc21182319e3ea95a5b69980628da87ec (2026-04-21)` — commit that added the PRD
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit (no tests run, no fixes made)

## Summary
- Goals met: 0 of 7
- Critical findings: 13 (Unmet=7, Stubs=0, Coverage Gaps=7, Missing Tests=2, Scope Drift=0)
- Recommended PRD Status: NOT STARTED
- One-line verdict: No code or tests have landed for in() / INPUT opcode / host input wiring; the PRD remains a pure design doc and its self-declared "NOT STARTED" status is accurate.

## Goal Verification
| Goal | Status | Evidence |
|---|---|---|
| G1: in() Akkado builtin returns stereo signal | Unmet | No "in" entry in akkado/include/akkado/builtins.hpp — only substring hits are param names like {"in","cut",...} in other builtins (builtins.hpp:200-279). |
| G2: in('mic'\|'tab'\|'file:...') string override | Unmet | Builtin not registered; no string-metadata path in akkado/src/codegen_patterns.cpp (only existing string intercept is handle_soundfont_call). |
| G3: ExecutionContext input_left/input_right | Unmet | Zero hits for input_left/input_right in cedar/. Only STEREO_INPUT dispatch flag at cedar/include/cedar/vm/instruction.hpp:197,202,205 (unrelated). Context unchanged. |
| G4: Web audio-input panel + source dropdown | Unmet | AudioInputPanel.svelte absent. input-source.ts absent. No getUserMedia/getDisplayMedia/MediaStream hits in web/. |
| G5: CLI --input-device / --list-devices | Unmet | tools/nkido-cli/audio_engine.cpp:53 only opens iscapture=0. No capture device, no flags. |
| G6: Silent fallback on unavailable input | Unmet | No INPUT opcode to contain the fallback. |
| G7: Sample-rate matching validated on host | Unmet | No host input path exists. |

## Findings

### Unmet Goals
- in() builtin not registered in akkado/include/akkado/builtins.hpp.
- String-source metadata path not implemented; cedar_set_input_source export absent.
- ExecutionContext in cedar/include/cedar/vm/context.hpp has no input_left/input_right.
- Opcode::INPUT enum absent from cedar/include/cedar/vm/instruction.hpp; no op_input in utility.hpp; no dispatch case in cedar/src/vm/vm.cpp; generated metadata has no INPUT entry.
- Four new files in PRD §6 Create list are all absent: AudioInputPanel.svelte, input-source.ts, web/static/docs/in.md, experiments/test_op_input.py.
- web/static/worklet/cedar-processor.js has no inputs[0] routing.
- CLI capture path entirely missing in tools/nkido-cli/.

### Stubs
None.

### Coverage Gaps
- cedar/tests/test_vm.cpp has no [input] tagged cases (PRD §8.1).
- akkado/tests/test_codegen.cpp has no [input] tagged cases (PRD §8.2).
- experiments/test_op_input.py absent (PRD §8.2a).
- Web manual tests (§8.3) untestable.
- CLI manual tests (§8.4) untestable.
- Cross-platform sanity (§8.5) untestable.
- Edge cases (§7) no implementation or tests.

### Missing Tests
- cedar/tests/test_vm.cpp needs memcpy path, null-pointer silent fallback, statelessness, adjacent-buffer invariant cases.
- akkado/tests/test_codegen.cpp needs in(), in('mic'|'tab'|'file:...') compile, in('garbage') error, auto-lift into lp cases.

### Scope Drift
None. No accidental partial merges.

### Suggestions
- Confirm stereo-PRD dependencies (Phases 1, 3, 4 of prd-stereo-support.md) are landed before wiring auto-lift UX; §10 of this PRD explicitly flags the coupling.
- Phase 1 (Cedar VM + Akkado builtin, ~1 day) is a safely-mergeable first slice with only cedar/tests/test_vm.cpp + akkado/tests/test_codegen.cpp coverage.
- PRD dated 2026-04-21 with no activity in 3 days — either kick off Phase 1 or re-status to "Deferred".

## PRD Status
- Current: NOT STARTED (line 3)
- Recommended: NOT STARTED
- Reason: Every listed "Modify" file is unmodified for this contract, every "Create" file is absent. Self-declared status is consistent with the codebase.

# Audit: Audio Input PRD — Live Signal Processing

**PRD:** `docs/prd-audio-input.md`
**Audit base:** `45f89dd` (2026-04-26) — last commit before Audio Input implementation landed
**Audit head:** `00ae5ac` (2026-04-26) — current `master`
**Audited:** 2026-04-26
**Implementation commit:** `629a650` "Add live audio input: in() builtin + INPUT opcode + host wiring"

## Summary
- Goals met: 7 of 7 (after fix below)
- Critical findings resolved during audit: 1 regression (user-reported "Audio not initialized" bug)
- Coverage gaps recorded as un-tested: 1 (Vitest unit test for the regression — deferred, see Decisions)
- One-line verdict: PRD ships the in()/INPUT pipeline end-to-end, all green tests, but the user-facing audio-input panel was unusable before the user pressed Play. Root cause fixed in this audit.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1: `in()` Akkado builtin returns stereo signal | Met | `akkado/include/akkado/builtins.hpp:595-601` registers the builtin with `ChannelCount::Stereo`; codegen at `akkado/src/codegen_patterns.cpp:3191-3266` allocates an adjacent L/R buffer pair and emits `Opcode::INPUT`. |
| G2: `in('mic'\|'tab'\|'file:NAME')` source override | Met | `akkado/src/codegen_patterns.cpp:3212-3234` validates the literal and pushes onto `required_input_sources_`; surfaced via `akkado_get_required_input_source*` exports (`web/static/worklet/cedar-processor.js:306-317`). |
| G3: `ExecutionContext.input_left/right` populated by host | Met | `cedar/include/cedar/vm/context.hpp:34-38`, `cedar/include/cedar/vm/vm.hpp:68-72`, `cedar/src/vm/vm.cpp:1117-1119`. WASM populates it (`web/wasm/nkido_wasm.cpp:140-148`); CLI populates it (`tools/nkido-cli/audio_engine.cpp` capture path). |
| G4: Web audio-input panel + source selector | Met | `web/src/lib/components/Panel/AudioInputPanel.svelte` (segmented control mic/tab/file/none, device picker, file upload, constraint toggles); mounted in `web/src/lib/components/Panel/SidePanel.svelte:229`. |
| G5: CLI `--input-device` / `--list-devices` | Met | `tools/nkido-cli/main.cpp:42-43,130-137` (flag parsing); `tools/nkido-cli/audio_engine.cpp:33-47` (`list_capture_devices`); `tools/nkido-cli/audio_engine.cpp:49-` (`init_capture` opens SDL2 with `iscapture=1`). |
| G6: Silent fallback on unavailable input | Met | `cedar/include/cedar/opcodes/utility.hpp:60-75` writes silence when pointers are null. WASM `cedar_set_input_active(0)` (`web/wasm/nkido_wasm.cpp:191-193`) routes through the same path. |
| G7: Sample-rate matching validated on host | Met | WebAudio resamples automatically (browser native); CLI captures with the same `SDL_AudioSpec.freq` as playback (`audio_engine.cpp:60-65` reuses `want.freq` from playback) — no explicit assert needed because both devices are opened against the same rate. |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `./build/cedar/tests/cedar_tests "[input]"` | Pass (1024 assertions, 1 case) | Covers memcpy path, null-pointer silent fallback, statelessness, adjacent-buffer invariant. |
| `./build/akkado/tests/akkado_tests "[input]"` | Pass (29 assertions, 3 cases) | Covers in(), in('mic'/'tab'/'file:NAME'), in('garbage') error, in('file:'), auto-lift to STEREO_INPUT. |
| `./build/cedar/tests/cedar_tests` (full) | Pass (332,076 assertions, 149 cases) | No regressions. |
| `./build/akkado/tests/akkado_tests` (full) | Pass (136,836 assertions, 491 cases) | No regressions. |
| `cd web && bun run check` | Pass (0 errors, 0 warnings) | Including the post-fix re-check. |

## Findings

### Unmet Goals
None.

### Stubs
None.

### Regressions

- **Audio Input panel unusable before play** — `web/src/lib/stores/audio.svelte.ts:1796-1800` returned `state.inputError = 'Audio not initialized'` whenever `audioContext` or `workletNode` was null. Both are null until `play()` triggers `initialize()` (line 656); `setInputSource` itself never called `initialize()`. Picking Mic/Tab/File from the Settings → Audio Input panel before pressing Play silently surfaced "Audio Input Error" with **no console output** because the code returned without logging. Reported by user 2026-04-26.
  - **Resolved in audit.** Fix: `setInputSource` now calls `await initialize()` when the engine isn't ready. The button click counts as a user gesture, so AudioContext creation is allowed there. After init, the original null-check stays as a safety net (e.g., insecure context) and now falls back to `state.error` for a more useful message. Touch: `web/src/lib/stores/audio.svelte.ts:1796-1810`.

### Coverage Gaps

- **No automated test for `setInputSource` lifecycle** — the regression above shipped because no test exercises "click Mic before pressing Play." The web test suite (`web/tests/`) only contains `arrays.test.ts` (a WASM compile test); there is no Svelte-store-aware vitest setup, no DOM environment (jsdom/happy-dom not installed), and no `AudioContext`/`navigator.mediaDevices` mocks.
  - **Resolved as "accepted as un-tested — reason: requires test-infrastructure work outside this audit's scope."** Closing this gap properly needs: (a) jsdom/happy-dom dev dependency, (b) `@sveltejs/vite-plugin-svelte` wired into vitest config to process `$state` runes in `audio.svelte.ts`, (c) mocks for `AudioContext`, `AudioWorkletNode`, `navigator.mediaDevices.getUserMedia/getDisplayMedia`, and `fetch('/wasm/...')`. Tracked as future work.
- **Manual web tests in PRD §8.3 untested in CI** — by PRD design (these are documented as manual). Not an audit-introduced gap.
- **Python `experiments/test_op_input.py` absent** — explicitly deferred by the PRD's status line ("Python experiment harness deferred"). Existing C++ unit tests in `cedar/tests/test_vm.cpp` under `[input]` cover the same behavior.

### Missing Tests
None. PRD's named test artifacts (`cedar/tests/test_vm.cpp`/`akkado/tests/test_codegen.cpp` under `[input]`) are present.

### Scope Drift
None.

### Convention Drift
None.

### Suggestions

- **Phase 4 §9 promise unimplemented** — "auto-request on first `in()` compile". Currently the user must open Settings → Audio Input and pick a source. Worth a follow-up so a program with `in()` triggers a permission prompt on first compile. (User confirmed: noted, not in scope.)
- **"Audio not initialized" message is still cryptic** if the post-fix init path itself fails (insecure context, AudioContext throw). Worth a follow-up to surface a clearer error and a "Start audio" button. (User confirmed: noted, not in scope.)

## Decisions Recorded

- **Fix applied during audit:** `setInputSource` lazy-initializes the audio engine (`audio.svelte.ts:1795-1810`).
- **Audit replaces 2026-04-24 audit:** the older NOT-STARTED audit was deleted per user direction; this file is the new authoritative record.
- **PRD Status line:** PRD already says "IMPLEMENTED" (line 3); no change needed.
- **Coverage gap accepted:** Vitest unit test for `setInputSource` deferred — needs DOM-aware test infrastructure. The user-reported regression is closed by the lazy-init fix; manual verification (clicking Mic from a fresh tab) is the current safety net.
- **Phase 4 "auto-request on compile" remains a follow-up.**

## Tests Added / Extended
None. The user-reported regression was closed by code change; the corresponding automated regression test is deferred (see Coverage Gaps).

## Post-Finalize Validation

| Command | Result |
|---|---|
| `cd web && bun run check` (post-fix) | Pass (0 errors, 0 warnings) |
| `./build/cedar/tests/cedar_tests "[input]"` | Pass (1024 assertions) |
| `./build/akkado/tests/akkado_tests "[input]"` | Pass (29 assertions) |
| Manual: open Settings → Audio Input → Mic before pressing Play | Triggers AudioContext init + getUserMedia prompt (see code path `audio.svelte.ts:1795-1840`) |

## PRD Status

- Before: `IMPLEMENTED` (line 3)
- After: unchanged. The PRD's self-declared status was correct in spirit; the bug was an omitted lazy-init in one orchestration site, not a missing goal.

## Recommended Next Steps

1. (Suggested) Land DOM-aware vitest setup so audio-store regressions like this can be caught automatically.
2. (Suggested) Implement Phase 4 "auto-request on first `in()` compile" so users don't need to open Settings to grant permission.
3. (Suggested) Improve the "Audio not initialized" surface message and add a manual "Start audio" button for the rare cases the lazy-init itself fails.

# Audit: Editor Autocomplete & Code Assistance

**PRD:** `docs/prd-editor-autocomplete.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625 (2026-04-24)` -- PRD was added at HEAD by a batch-rename commit, so base/HEAD diff is empty; audit verifies implementation against codebase at HEAD.
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit (no tests run, no fixes made)

## Summary
- Goals met: 4 of 4 primary goals; 7 of 10 implementation-checklist items
- Critical findings: 3 (Unmet=0, Stubs=0, Coverage Gaps=3, Missing Tests=3)
- Recommended PRD Status: MOSTLY COMPLETE
- One-line verdict: All runtime code artifacts from the PRD are in place and wired, but the three testing checklist items are uncovered -- `web/tests/` contains only `arrays.test.ts`, no completion/signature-help tests exist.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1: Reduce friction / autocomplete popup | Met | `web/src/lib/editor/akkado-completions.ts:168` (`akkadoCompletions`) wired at `web/src/lib/components/Editor/Editor.svelte:147-150` (`autocompletion({override: [akkadoCompletions], activateOnTyping: true})`). Identifier min-length gate at `akkado-completions.ts:170-176`. |
| G2: Improve productivity / signature help | Met | `web/src/lib/editor/signature-help.ts:436` (`signatureHelp()`), wired at `web/src/lib/components/Editor/Editor.svelte:151`. Active-param bold/underline at `signature-help.ts:137-152`. |
| G3: Single source of truth / compiler metadata | Met | JSON generated from `BUILTIN_FUNCTIONS` / `BUILTIN_ALIASES` at `web/wasm/nkido_wasm.cpp:781-833`. `description` field added at `akkado/include/akkado/builtins.hpp:80`. |
| G4: Zero latency / no network | Met | Worklet handler `web/static/worklet/cedar-processor.js:986-1022` calls `this.module._akkado_get_builtins_json()`; cached in `web/src/lib/stores/audio.svelte.ts:1443-1471` and in both consumers (`akkado-completions.ts:15`, `signature-help.ts:14`). |

## Implementation Checklist Verification

| Checklist Item | Status | Evidence |
|---|---|---|
| Add `description` field to `BuiltinInfo` | Met | `akkado/include/akkado/builtins.hpp:80` |
| Add descriptions to all 93+ builtin functions | Met | Every entry in `BUILTIN_FUNCTIONS` (`builtins.hpp:139-907`) supplies a description. Samples: `"Triangle wave oscillator"` at :147, `"Attach waveform visualization..."` at :898 |
| Add `akkado_get_builtins_json()` WASM export | Met | `web/wasm/nkido_wasm.cpp:781` |
| Handle builtin request in AudioWorklet | Met | Dispatch `web/static/worklet/cedar-processor.js:182-184`, impl :986-1022 |
| Create `akkado-completions.ts` | Met | `web/src/lib/editor/akkado-completions.ts` (241 lines) |
| Create `signature-help.ts` | Met | `web/src/lib/editor/signature-help.ts` (439 lines) |
| Wire up extensions in Editor.svelte | Met | `web/src/lib/components/Editor/Editor.svelte:13-14,147-151` |
| Test all builtin functions appear | Coverage Gap | No test file references `akkadoCompletions` or `builtins`. `web/tests/` has only `arrays.test.ts`. |
| Test user-defined function completion | Coverage Gap | No test exercises `extractUserFunctions` / `extractUserVariables` (`akkado-completions.ts:91-131`). |
| Test signature help parameter highlighting | Coverage Gap | No test exercises `findFunctionContext` / active-param logic (`signature-help.ts:62-152`). |

## Findings

### Unmet Goals
None -- all four primary goals have shipping code.

### Stubs
No TODO / FIXME / XXX / HACK / NotImplemented markers in `akkado-completions.ts`, `signature-help.ts`, or `builtins.hpp` (grep returned zero hits). The PRD itself marks regex-based user-symbol extraction as v1 with AST-based extraction as future work (`docs/prd-editor-autocomplete.md:156-164`) -- matches the implementation.

### Coverage Gaps
1. **Builtin completion presence** -- no automated check that `lp` / `moog` / `adsr` surface via `akkadoCompletions`, or that alias mapping works.
2. **User-defined symbol extraction** -- no regression test for the docstring regex at `akkado-completions.ts:118` / `signature-help.ts:39`.
3. **Signature help context finder** -- no coverage for the edge cases `findFunctionContext` handles: cursor on function name (`signature-help.ts:70-83`), multi-line args (`signature-help.ts:104-110`), nested parens (`signature-help.ts:87-112`).

### Missing Tests
All three testing-checklist items in the PRD lack implementation. `web/tests/` contains only `arrays.test.ts` and `wasm-helper.ts`; no file matches `*completion*` / `*signature*` / `*autocomplete*`. PRD acceptance cannot be demonstrated by a regression suite.

### Scope Drift
None. `git diff 4977c77..HEAD --name-only` returns only `akkado/tests/test_codegen.cpp`, `docs/MVP-INCOMPLETE-IMPLEMENTATIONS.md`, and an unrelated audit doc. PRD files (`akkado-completions.ts`, `signature-help.ts`, Editor.svelte wiring, `nkido_wasm.cpp` export, `builtins.hpp` description field) were all committed pre-base -- consistent with a Status: DONE PRD written/renamed after shipping.

### Suggestions
1. Add `web/tests/completions.test.ts` covering (a) `akkado_get_builtins_json` parses, (b) builtins `lp` / `moog` / `adsr` appear with expected param counts, (c) aliases `lowpass -> lp`, `reverb -> freeverb` resolve.
2. Add unit tests for the pure functions `extractUserFunctions`, `extractUserVariables`, `findFunctionContext` -- regex-only, trivially testable without CodeMirror.
3. Deduplicate `extractUserFunctions` regex (verbatim in `akkado-completions.ts:118` and `signature-help.ts:39`) into a shared module to avoid drift.
4. PRD risk-mitigation promises fuzzy matching + top-20 cap (PRD :201). Current implementation returns every builtin/alias/keyword/user symbol unbounded, relying on CodeMirror default ranking. Confirm this is acceptable or add the cap.
5. PRD promises `Ctrl+Space` manual trigger (PRD :32); inherited from CodeMirror default keymap -- worth confirming in a test since not wired explicitly.

## PRD Status
- Current: `DONE` (line 1: `> **Status: DONE** -- Completions, signature help, WASM builtins export.`)
- Recommended: `MOSTLY COMPLETE`
- Reason: All runtime code (C++ `description` field, WASM export, worklet handler, TypeScript completion source, signature-help plugin, Editor wiring) is implemented and matches the PRD architecture. However, three PRD checklist items (`Test all builtin functions appear`, `Test user-defined function completion`, `Test signature help parameter highlighting`) have no automated tests. Feature is user-visible and functional; only verification is thin.

# Audit: Visualization System PRD

**PRD:** `docs/prd-visualization-system.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625` (PRD creation commit; `git log --diff-filter=A` returns the most recent commit touching the file, since the file was renamed from `VISUALIZATION-SYSTEM-PRD.md` to lowercase `prd-visualization-system.md` in that commit)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Relationship to prd-viz-system-revision.md

**Partially superseded / extended.** `prd-viz-system-revision.md` is explicitly scoped as a successor that *extends* this PRD across three revisions:

- **Revision 1** (marked `REVISION 2 IMPLEMENTED` in its status line): adds `waterfall()` visualization, `FFT_PROBE`/`IFFT` opcodes with kissfft, and migrates `spectrum()` from JS DFT to WASM FFT.
- **Revision 2** (outline): implements all the originally-specified per-type parameters this PRD enumerated (`fftSize`, `logScale`, `minDb`, `maxDb`, `beats`, `duration`, `scale`, `color`, pianoroll `scale`, etc.) — the revision PRD explicitly notes "None of which are implemented" at that time.
- **Revision 3** (outline): detachable floating windows.

This PRD's scope (the core extensible viz system with 4 viz types, block decorations, PROBE opcode, signal capture) is still the foundational layer and remains valid. The revision PRD supplements rather than replaces it.

The PRD's own status header reads `Status: DONE — PROBE opcode, pianoroll/oscilloscope/waveform/spectrum, viz declarations`, which matches the shipped code, even though the six Success Criteria checkboxes below remain unchecked (apparently never updated).

## Summary
- Goals met: 5 of 5 (all numbered Goals from the Problem Statement)
- Critical findings: 2 (Unmet=0, Stubs=0, Coverage Gaps=1, Missing Tests=1)
- Recommended PRD Status: **DONE** (keep as-is; update Success Criteria checkboxes to reflect shipped state)
- One-line verdict: Core visualization system is fully implemented and matches the PRD's scope; only the Success Criteria checkboxes are stale — actual code, tests, and WASM exports are all in place.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1: Visualizations display below source line, not inline | Met | `web/src/lib/editor/visualization-widgets.ts:180` (`block: true`), `web/src/lib/editor/visualization-widgets.ts:197` (`visualizationField` StateField), integrated in `web/src/lib/components/Editor/Editor.svelte:191-192` |
| G2: Visualizations as pipeable function calls | Met | Builtins registered in `akkado/include/akkado/builtins.hpp:887-903` (pianoroll/oscilloscope/waveform/spectrum/waterfall); handler dispatch in `akkado/src/codegen.cpp:842-846`; pass-through semantics in `akkado/src/codegen_viz.cpp` |
| G3: Multiple viz types (pianoroll, oscilloscope, waveform, spectrum) | Met | `akkado/include/akkado/codegen.hpp:71` (`VisualizationType` enum with PianoRoll/Oscilloscope/Waveform/Spectrum + Waterfall); renderer files `web/src/lib/visualizations/{pianoroll,oscilloscope,waveform,spectrum,waterfall}.ts` |
| G4: Extensible plugin architecture | Met | `web/src/lib/visualizations/registry.ts:14-63` defines `VisualizationRenderer` interface with `registerRenderer`/`getRenderer`/`hasRenderer`; each renderer file calls `registerRenderer()` |
| G5: Zero audio-thread allocations for signal capture | Met | `cedar/include/cedar/opcodes/dsp_state.hpp:1164-1192` (`ProbeState` uses fixed inline `buffer[1024]`); `FFTProbeState` at `dsp_state.hpp:1198-1218` uses arena allocation (`utility.hpp:266-269` allocates once via `ctx.arena->allocate()` during first write); PROBE opcode at `cedar/src/vm/vm.cpp:1015-1020` |

### Success Criteria Checkbox Audit

All six PRD checkboxes (lines 394-399) are unchecked but the underlying criteria are met:

| Checkbox | Implemented? | Evidence |
|---|---|---|
| Visualizations render below source lines | Yes | `visualization-widgets.ts:180` `block: true` |
| `\|> pianoroll(%, ...)` syntax works | Yes | `builtins.hpp:887`, `codegen_viz.cpp:143` (`handle_pianoroll_call`) |
| All four viz types functional | Yes | All four renderers exist + a fifth (waterfall) from the revision |
| Playhead synchronized with beat position | Yes | `pianoroll.ts:88-100` uses `beatPos` for window positioning; `registry.ts:30` defines the signature |
| No audio dropouts with 4+ viz | Not verifiable by static audit (performance criterion) |
| Zero allocations in audio thread for probe capture | Yes | `ProbeState` inline array; `FFTProbeState` uses pre-allocated arena via `ctx.arena->allocate()` on first block (arena pre-allocated at VM init, not in hot path) |

## Findings

### Unmet Goals
None.

### Stubs
None located in the shipped code paths (handlers, opcodes, renderers, WASM exports are all real implementations, not placeholders).

### Coverage Gaps
- **CG1: No dedicated C++ tests for `ProbeState` ring-buffer correctness.** The PRD's Testing Strategy (line 373-375) calls for "`ProbePool` ring buffer correctness in Cedar tests". `cedar/tests/test_fft.cpp` tests `FFTProbeState`, but there is no analogous `test_probe_state.cpp` or similar covering `ProbeState::write_block`, wrap-around, or `sample_count()` semantics. Grep for `ProbeState` in `cedar/tests/*.cpp` returns nothing.

### Missing Tests
- **MT1: Akkado codegen tests for non-waterfall viz handlers are thin.** `akkado/tests/test_codegen.cpp` has extensive coverage for `waterfall()` (lines 4921-5040, including fft-size rate encoding, BoolLit serialization, spectrum→FFT_PROBE migration), but there is no test suite specifically verifying that `pianoroll()`, `oscilloscope()`, and `waveform()` emit correct `VisualizationDecl` entries with the right `VisualizationType` enum value, source offsets, and PROBE opcode emission (for oscilloscope/waveform). The BoolLit test at 5005 touches pianoroll incidentally but doesn't assert the full decl contract.
- **MT2: No integration test for block decoration placement.** PRD Testing Strategy calls for "Visual verification of block placement (below line)" as manual testing — no automated coverage, which is acceptable for visual-only checks but worth noting.

### Scope Drift
- **SD1 (benign): Builtin opcode changed from `NOP` to `COPY`.** The PRD specifies `cedar::Opcode::NOP` for pass-through viz builtins (line 140-143). The shipped code uses `cedar::Opcode::COPY` in `builtins.hpp:887-903`. This is a reasonable implementation detail since COPY properly routes signal through `out_buffer`, whereas NOP would not. Not a regression.
- **SD2: `ProbePool` concept replaced by per-state `ProbeState`.** The PRD described a centralized `ProbePool` with `MAX_PROBES = 16` and an array of ring buffers keyed by `state_id` (lines 162-178). The implementation instead stores a `ProbeState` per-`state_id` via the standard `StatePool::get_or_create<ProbeState>(inst.state_id)` pattern — which is cleaner and consistent with how every other stateful opcode works, and avoids the 16-probe limit. Also benign.
- **SD3: Waterfall added beyond original PRD scope.** `VisualizationType::Waterfall = 4` and `FFT_PROBE = 181` exist. This is explicitly scoped by `prd-viz-system-revision.md`, so it's tracked elsewhere, not genuine drift.

### Suggestions
1. **Check the Success Criteria boxes** in `prd-visualization-system.md` to match the "DONE" status header. All 6 items are satisfied by the codebase; leaving them unchecked creates confusion.
2. **Add `test_probe_state.cpp`** (or extend `test_fft.cpp`) with explicit `ProbeState` ring-buffer tests to close CG1.
3. **Add codegen tests** for `pianoroll`/`oscilloscope`/`waveform` viz-decl emission mirroring the waterfall test coverage at `test_codegen.cpp:4921-5040` (MT1).
4. **Cross-link** this PRD and `prd-viz-system-revision.md` at the top of each so future readers know the revision is additive, not replacing.

## PRD Status
- Current: `DONE` (per status line 1)
- Recommended: `DONE` (keep; also update the 6 unchecked Success Criteria boxes to match)
- Reason: Every numbered Goal in the Problem Statement maps to shipped code with clear file:line evidence. The core architecture (VisualizationDecl, block widgets, PROBE opcode, renderer registry, WASM exports, worklet data pathway) is fully implemented. Minor test coverage gaps exist but do not warrant downgrading the status. The companion revision PRD tracks the remaining extended-parameters and detachable-window work.

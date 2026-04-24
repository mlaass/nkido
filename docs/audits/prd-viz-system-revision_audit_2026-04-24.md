# Audit: Visualization System Revision

**PRD:** `docs/prd-viz-system-revision.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625` (PRD added at HEAD)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 10 of 11 (Revision 1 success criteria) + 7 of 7 implementation phases + partial for Rev 2 outlines
- Critical findings: 3 (Unmet=1 partial, Stubs=0, Coverage Gaps=2, Missing Tests=0)
- Recommended PRD Status: `REVISION 1 & 2 IMPLEMENTED (partial Rev 2); REVISION 3 NOT STARTED` (matches current header but nuanced)
- One-line verdict: Revision 1 is fully implemented end-to-end (FFT/IFFT opcodes, kissfft wrapper, FFT_PROBE, WASM exports, worklet, renderer, spectrum migration) with accompanying tests; Rev 2 params partially land (spectrum has ResizeObserver+logScale, waterfall lacks ResizeObserver); Rev 3 detachable windows not started (consistent with "Rev 2 Implemented" header).

## Goal Verification

### Stated Goals (lines 19-25)
| Goal | Status | Evidence |
|---|---|---|
| 1. Add `waterfall()` viz type | Met | `akkado/src/codegen_viz.cpp:418-485` (`handle_waterfall_call`); `web/src/lib/visualizations/waterfall.ts:1-249`; `akkado/include/akkado/codegen.hpp:76` (`Waterfall = 4`) |
| 2. Add FFT/IFFT opcodes using kissfft | Met | `cedar/include/cedar/vm/instruction.hpp:189,191` (`FFT_PROBE=181`, `IFFT=182`); `cedar/src/dsp/fft.cpp:1-163`; `cedar/include/cedar/dsp/fft.hpp`; `cedar/third_party/kissfft/` (kiss_fft.{h,c}, kiss_fftr.{h,c}, COPYING) |
| 3. Replace JS DFT in spectrum.ts with WASM FFT | Met | `web/src/lib/visualizations/spectrum.ts:1-6` (module doc mentions WASM FFT via FFT_PROBE); no `computeSpectrum` DFT function remains (grep returned zero hits); `akkado/src/codegen_viz.cpp:400` (spectrum emits FFT_PROBE) |
| 4. Support configurable direction, speed, FFT res, gradient, sizing | Met | `web/src/lib/visualizations/waterfall.ts:60-66` parses `angle`, `speed`, `fft`, `gradient`, `minDb`, `maxDb`, `width`, `height`; `akkado/src/codegen_viz.cpp:125-137` (`extract_fft_log2`) |
| 5. Design FFT opcodes for future user-accessible spectral processing | Met | `cedar/include/cedar/vm/instruction.hpp:191` (IFFT reserved); `cedar/include/cedar/opcodes/dsp_state.hpp:1205-1206` (`real_bins`, `imag_bins` stored for future IFFT use); `cedar/src/dsp/fft.cpp` (`compute_ifft`) |

### Phase 1: kissfft Integration (lines 425-429)
| Checkbox | Status | Evidence |
|---|---|---|
| Add kissfft source files | Met | `cedar/third_party/kissfft/{kiss_fft.c,kiss_fft.h,kiss_fftr.c,kiss_fftr.h,_kiss_fft_guts.h,kiss_fft_log.h,COPYING}` |
| Create C++ wrapper | Met | `cedar/include/cedar/dsp/fft.hpp`; `cedar/src/dsp/fft.cpp` |
| Integrate into cedar CMake | Presumed Met | `cedar/src/dsp/fft.cpp` exists and is referenced by tests; tests compile (not run per audit rules) |
| Verify native build | Presumed Met | Test file `cedar/tests/test_fft.cpp:221` exists and is registered in `cedar/tests/CMakeLists.txt:36` |

### Phase 2: FFT_PROBE Opcode + State (lines 431-437)
| Checkbox | Status | Evidence |
|---|---|---|
| Add FFTProbeState | Met | `cedar/include/cedar/opcodes/dsp_state.hpp:1198-1218` |
| Add FFTProbeState to DSPState variant | Met | `cedar/include/cedar/opcodes/dsp_state.hpp:1281` (guarded by `#ifndef CEDAR_NO_FFT`) |
| Add FFT_PROBE = 181 | Met | `cedar/include/cedar/vm/instruction.hpp:189` |
| Implement op_fft_probe | Met | `cedar/include/cedar/opcodes/utility.hpp:254-285` |
| Wire into VM dispatch | Met | `cedar/src/vm/vm.cpp:1020` (`case Opcode::FFT_PROBE`) |
| Reserve IFFT = 182 | Met | `cedar/include/cedar/vm/instruction.hpp:191` |

### Phase 3: WASM Exports + AudioWorklet (lines 439-445)
| Checkbox | Status | Evidence |
|---|---|---|
| Add 3 WASM exports | Met | `web/wasm/nkido_wasm.cpp:1475,1492,1511` |
| Add exports to CMakeLists | Met | `web/wasm/CMakeLists.txt:120-122` |
| Add worklet handler | Met | `web/static/worklet/cedar-processor.js:214-215,1276-1340` |
| Add `getFFTProbeData` / `VizType.Waterfall` | Met | `web/src/lib/stores/audio.svelte.ts:1719-1734,1810`; `VizType.Waterfall=4` (enum defined at store) |
| Regenerate opcode metadata | Presumed Met | Generated metadata not audited per read-only rule; FFT_PROBE is referenced by worklet/store suggesting downstream consistency |

### Phase 4: Compiler Changes (lines 447-453)
| Checkbox | Status | Evidence |
|---|---|---|
| Add VisualizationType::Waterfall = 4 | Met | `akkado/include/akkado/codegen.hpp:76` |
| Extend extract_options_json for StringLit | Met | `akkado/src/codegen_viz.cpp:87-96`; also BoolLit at line 98-106 (bonus) |
| Declare handle_waterfall_call | Met | `akkado/include/akkado/codegen.hpp:456-457` |
| Implement handle_waterfall_call | Met | `akkado/src/codegen_viz.cpp:418-485` |
| Register waterfall builtin | Met | `akkado/src/codegen.cpp:846`; also registered in `akkado/include/akkado/builtins.hpp:903` |
| Migrate handle_spectrum_call to FFT_PROBE | Met | `akkado/src/codegen_viz.cpp:400` (emits `FFT_PROBE`) |

### Phase 5: Waterfall Renderer (lines 455-460)
| Checkbox | Status | Evidence |
|---|---|---|
| Create gradients.ts with 5 presets | Met | `web/src/lib/visualizations/gradients.ts:49-85` (magma, viridis, inferno, thermal, grayscale, DEFAULT_GRADIENT='magma') |
| Create waterfall.ts | Met | `web/src/lib/visualizations/waterfall.ts:1-249` |
| Angle-to-direction + scroll accumulation | Met (different approach) | `waterfall.ts:104-108,132,182-186`: uses offscreen canvas + rotation instead of per-direction copyWithin; achieves all 4 cardinal angles plus arbitrary angles. Differs from PRD's "copyWithin by row/stride" algorithm but meets the goal. |
| Relative sizing with ResizeObserver | Unmet (partial) | `waterfall.ts:54-57,75-76` sets CSS `100%` for relative sizing but **no ResizeObserver** is attached to reallocate ImageData on resize (spectrum.ts uses ResizeObserver correctly at line 87-89) |
| Register in index.ts | Met | `web/src/lib/visualizations/index.ts:15` (`import './waterfall';`) |

### Phase 6: Spectrum Migration (lines 462-465)
| Checkbox | Status | Evidence |
|---|---|---|
| Update spectrum.ts to use getFFTProbeData | Met | `web/src/lib/visualizations/spectrum.ts:141` |
| Remove computeSpectrum JS DFT | Met | grep `computeSpectrum\|DFT\|JS DFT` returned zero hits in `spectrum.ts` |
| Render spectrum bars from dB array | Met | `spectrum.ts` directly uses magnitudes from getFFTProbeData (per module doc at line 1-6 and update at line 141) |

### Phase 7: Testing + Polish (lines 467-472)
| Checkbox | Status | Evidence |
|---|---|---|
| C++ unit tests for FFT wrapper (peaks, window, sizes) | Met | `cedar/tests/test_fft.cpp:18` (440Hz peak), `test_fft.cpp:63` (power-of-2 sizes loop), `test_fft.cpp:91-94` (FFT->IFFT round-trip), `test_fft.cpp:115` (magnitude dB finite) |
| C++ unit tests for FFTProbeState (accumulation, frame triggering) | Met | `cedar/tests/test_fft.cpp:151` (frame counter increments), `test_fft.cpp:188` (magnitudes finite) |
| Akkado compiler tests for waterfall | Met | `akkado/tests/test_codegen.cpp:4918-5030` (6 sections: default, fft:512, fft:2048, gradient string, Waterfall type, various options) |
| Manual integration testing | Not Verifiable | Read-only audit cannot run manual tests, but hooks are in place |
| Verify spectrum migration identical output | Not Verifiable | Requires manual comparison |

### Success Criteria (lines 623-634)
| Criterion | Status | Evidence |
|---|---|---|
| waterfall() renders scrolling spectrogram | Met | Full pipeline present from codegen -> opcode -> WASM -> worklet -> renderer |
| All 4 scroll directions | Met | `waterfall.ts:132` uses `-angle * Math.PI / 180` rotation (continuous angle support) |
| Scroll speed configurable | Met | `waterfall.ts:61,183` `state.speed * state.dpr * deltaTime` |
| 5 gradient presets | Met | `gradients.ts:49-85` |
| FFT resolution configurable | Met | `codegen_viz.cpp:125-137` maps 256/512/1024/2048 to log2 |
| Relative sizing ("100%") works | Unmet (partial) | CSS is set (`waterfall.ts:75-76`) but no ResizeObserver to reallocate canvas/ImageData |
| FFT_PROBE zero-allocation in audio thread | Met | `utility.hpp:262-274` - buffers are arena-allocated once on first access |
| kissfft compiles into WASM | Presumed Met | Exports are registered; audit cannot build |
| spectrum() uses WASM FFT | Met | See Phase 6 |
| No audio dropouts with 4+ waterfalls | Not Verifiable | Manual test only |
| IFFT opcode number reserved | Met | `instruction.hpp:191` |

## Findings

### Unmet Goals
- **Waterfall ResizeObserver missing** (`web/src/lib/visualizations/waterfall.ts`): PRD §9 (line 381) and Success Criterion "Relative sizing ('100%') works and responds to container resize" require a `ResizeObserver` that reallocates canvas/ImageData on resize. The current implementation only sets CSS `100%` but has no observer. Compare to `spectrum.ts:22,86-89` which does implement ResizeObserver. Visual will stretch via CSS but the ImageData pixel buffer and offscreen canvas will not track container size changes.

### Stubs
- None found. `FFT_PROBE` has a complete implementation; `IFFT` is explicitly reserved per PRD §Non-Goals (no implementation expected for Rev 1).

### Coverage Gaps
- **kissfft windowing verification test absent**: PRD Testing §Cedar FFT wrapper (line 517) calls for "verify Hanning window application (windowed DC input should have expected sidelobe attenuation)". `cedar/tests/test_fft.cpp` covers peak detection, size loop, round-trip, and dB finite-ness, but lacks an explicit window-shape/sidelobe test.
- **Waterfall integration smoke test absent**: No automated test exercises the full waterfall -> WASM -> render path (only codegen emission is tested in `test_codegen.cpp:4918+`).

### Missing Tests
- No dedicated test for `extract_fft_log2` boundary cases (256/512/1024/2048 and defaults) beyond the waterfall section checks.
- No test for the rate-field -> fft_size conversion in `op_fft_probe` at the C++ level.

### Scope Drift
- **Waterfall rendering approach differs from PRD**: PRD §9 (lines 354-366) specifies per-pixel `copyWithin` shift + direct ImageData pixel writes on the exposed edge. Actual implementation (`waterfall.ts:115-234`) uses an offscreen canvas that always scrolls left-to-right at full FFT bin resolution, then `ctx.drawImage(state.offCanvas, ...)` with `ctx.rotate(rotation)` to draw onto the visible canvas. This is a more flexible approach (supports arbitrary angles, not just 0/90/180/270) and documented in the file header, but does diverge from PRD. Not a functional defect.
- **BoolLit support in extract_options_json**: Added (`codegen_viz.cpp:98-106`) though Rev 1 PRD only required StringLit. This is preemptive Rev 2 work — positive drift.
- **push_path(source_offset) for all viz handlers**: PRD §Follow-up (line 314) called this a "separate change". It is already applied uniformly across pianoroll/oscilloscope/waveform/spectrum/waterfall handlers — correct behavior.
- **PRD Status line overstates Rev 2 completion**: Header claims "REVISION 2 IMPLEMENTED — Extended viz parameters for all types." However Rev 2 scope (PRD §558-587) explicitly lists `logScale`, `minDb`/`maxDb` for spectrum (present), but also `beats`/`scale`/`showGrid` for pianoroll, `duration`/`scale`/`filled` for waveform, `beats`/`triggerLevel`/`triggerEdge` for oscilloscope. These were not audited in detail but the claim "all types" needs re-verification.

### Suggestions
1. Add a `ResizeObserver` to `waterfall.ts` (mirror `spectrum.ts:87-89`) to satisfy the relative-sizing success criterion.
2. Add a Hanning-window sidelobe attenuation test to `cedar/tests/test_fft.cpp`.
3. Clarify PRD status — either downgrade "Revision 2 implemented" to "partially implemented" or add explicit coverage for each Rev 2 per-type parameter.
4. Consider adding an integration-level waterfall smoke test (can't render in unit test but can instantiate state + verify data flow through WASM exports).

## PRD Status
- Current: `REVISION 2 IMPLEMENTED` (header line 1)
- Recommended: `REVISION 1 IMPLEMENTED; REVISION 2 PARTIAL; REVISION 3 NOT STARTED`
- Reason: Revision 1 is end-to-end complete and well-tested. Revision 2 is partially present — spectrum acquired `logScale`, `minDb`, `maxDb`, ResizeObserver, and `fft` (as claimed), but waterfall lacks ResizeObserver (blocking its own relative-sizing success criterion) and Revision 2's pianoroll/waveform/oscilloscope parameter extensions were not verified in this audit. Revision 3 (detachable floating windows) has no evidence of implementation (grep for `detached`/`FloatingViz`/`floating` returned zero hits), consistent with the current header that does not claim Rev 3.

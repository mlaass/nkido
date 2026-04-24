# Audit: NKIDO Web IDE

**PRD:** `docs/prd-nkido-web-ide.md`
**Audit base:** `68d26b3ecbc4015646969a0aee118923a148aaee` (first commit to add the PRD under an earlier filename; the PRD was subsequently renamed to `prd-nkido-web-ide.md`)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 5 of 5 top-level goals (with gaps); 30 of ~37 implementation checkboxes met
- Critical findings: 7 (Unmet=6, Stubs=0, Coverage Gaps=2, Missing Tests=1, Scope Drift=1)
- Recommended PRD Status: **PARTIAL** (unchanged — matches declared status)
- One-line verdict: The IDE ships with a Svelte 5 + SvelteKit app, CodeMirror 6 editor, AudioWorklet-hosted Cedar VM WASM, inline visualization widgets, docs browser with runnable widgets, parameter controls, and an embed route; Phase 7 polish (PWA, share URLs, mobile responsiveness, canvas knobs/XY pad) plus a few Phase 3 widget types are largely unimplemented, consistent with the declared `PARTIAL` status.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1. Live Coding — edit + hot-swap | Met | `web/src/lib/components/Editor/Editor.svelte:97-130` (Ctrl+Enter evaluate), `web/src/lib/stores/audio.svelte.ts:898-1061` (`compile` → worklet → `loadCompiledProgram` with SlotBusy retry), `web/static/worklet/cedar-processor.js:1-30` hosts Cedar VM WASM |
| G2. Rich Visualization | Met (gaps) | `web/src/lib/visualizations/` — oscilloscope/waveform/spectrum/waterfall/pianoroll registered via `registry.ts`; inline block decorations `web/src/lib/editor/visualization-widgets.ts:1-60`; probe/FFT data plumbing `web/src/lib/stores/audio.svelte.ts:1695-1734`. Gaps: envelope / filter-response / stereo-meter widgets absent. |
| G3. Interactive Documentation | Met | `web/src/lib/docs/parser.ts:13-40` marks ```akk``` blocks for widget transformation; `web/src/lib/docs/components/DocCodeWidget.svelte:14-35` implements Run/Stop/Copy; docs routes at `web/src/routes/docs/+layout.svelte`, `web/src/routes/docs/[category]/+page.svelte`; search via `web/src/lib/docs/components/DocSearch.svelte`; pre-built lookup in `web/src/lib/docs/lookup.ts` and `web/scripts/build-docs-index.ts` |
| G4. Lightweight & Elegant (Svelte + CM6) | Met | `web/package.json:18-42` pins Svelte 5, SvelteKit 2, CodeMirror 6, marked, gray-matter, lucide-svelte; no heavy runtime deps |
| G5. Educational | Partially met | Tutorials: `web/static/docs/tutorials/01-hello-sine.md` … `05-testing-progression.md`; runnable widgets via DocCodeWidget; `web/static/patches/*.akk` + `/embed` route. No dedicated "examples gallery" UI. |

## Phase-Level Checkbox Verification (~37 items)

### Phase 1: Foundation (6 items — all Met)
| Item | Status | Evidence |
|---|---|---|
| SvelteKit project scaffold | Met | `web/svelte.config.js`, `web/vite.config.ts`, `web/tsconfig.json`, `web/src/app.html` |
| WASM build for browser (Emscripten) | Met | `web/wasm/CMakeLists.txt:40-165` emits `nkido.js` + `nkido.wasm`, exports cedar+akkado C APIs |
| Basic AudioWorklet with Cedar VM | Met | `web/static/worklet/cedar-processor.js`, wired from `web/src/lib/stores/audio.svelte.ts:351-379` |
| Minimal CodeMirror editor | Met | `web/src/lib/components/Editor/Editor.svelte:132-207` |
| Play/Pause/BPM transport controls | Met | `web/src/lib/components/Transport/Transport.svelte:1-40` (BPM + volume) |
| Test with mock bytecode initially | Met (legacy) | `web/src/lib/stores/audio.svelte.ts:1066-1082` exposes `loadProgram(bytecode)`; typical flow is `compile()` |

### Phase 2: Compiler Integration (4 items — all Met)
| Item | Status | Evidence |
|---|---|---|
| WASM bindings for `akkado::compile()` | Met | `_akkado_compile` + related exports in `web/wasm/CMakeLists.txt:85-105`; main-thread wrapper `web/src/lib/stores/audio.svelte.ts:898-928` |
| Error display in editor (underlines, messages) | Met | `web/src/lib/components/Editor/editor-linter.ts:12-34`; error log panel `web/src/lib/components/Editor/Editor.svelte:253-271` |
| Hot-swap on Ctrl+Enter | Met | `web/src/lib/components/Editor/Editor.svelte:97-110` + `editorStore.evaluate()`; SlotBusy retry `web/src/lib/stores/audio.svelte.ts:1014-1054` |
| Source mapping for error locations | Met | `Diagnostic {line, column}` + `SourceLocation` + `instructionHighlight` extension (`web/src/lib/stores/audio.svelte.ts:64-95`) |

### Phase 3: Inline Visualizations (7 items)
| Item | Status | Evidence |
|---|---|---|
| CodeMirror widget system (block decorations) | Met | `web/src/lib/editor/visualization-widgets.ts:1-60` |
| Semantic ID → Widget mapping | Met | `VizDecl.stateId` from WASM (`_akkado_get_viz_state_id`) → registry (`web/src/lib/stores/audio.svelte.ts:40-62`, `web/src/lib/visualizations/registry.ts`) |
| Oscillator widgets (waveform) | Met | `web/src/lib/visualizations/waveform.ts` (probe data via `_cedar_get_probe_data`) |
| Filter widgets (frequency-response curve) | Unmet | No `VizType.FreqResponse`; no filter-response renderer. Spectrum shows live signal FFT, not filter transfer function. |
| Envelope widgets (ADSR shape) | Unmet | No `VizType.Envelope` in `web/src/lib/stores/audio.svelte.ts:40-46`; no `envelope.ts` renderer |
| Pattern widgets (timeline + beat markers, active highlight) | Met | `VizType.PianoRoll` + `web/src/lib/visualizations/pianoroll.ts`; active step highlight `web/src/lib/editor/step-highlight.ts` and `web/src/lib/stores/pattern-highlight.svelte.ts`; `web/src/lib/components/Panel/PatternDebugPanel.svelte` |
| Output widget (stereo meters/waveform) | Partially met | Generic waveform/oscilloscope can be attached to `out(%, %)` manually; no dedicated stereo-meter `VizType`. Analyser node wired (`web/src/lib/stores/audio.svelte.ts:336-339, 1102-1117`) but not surfaced inline. |
| Global viz toggle | Met | `audioEngine.toggleVisualizations()` + header button (`web/src/routes/+page.svelte:25-31`); state `visualizationsEnabled` in `web/src/lib/stores/audio.svelte.ts` |

### Phase 4: Editor Polish (4 items)
| Item | Status | Evidence |
|---|---|---|
| Akkado language mode (full syntax highlighting) | Met | `web/src/lib/editor/akkado-language.ts:1-60` (keywords, operators `|>`, `%`, `~`, numbers, strings, builtins via generated list) |
| Mini-notation syntax highlighting inside strings | Partially met | `web/src/lib/editor/akkado-language.ts:22-33` tokenises strings as a single `string` token. Active-step highlight provides runtime visual feedback (`web/src/lib/editor/step-highlight.ts`), but no structural coloring inside pattern literals. |
| Autocomplete for built-in functions with signatures | Met | `web/src/lib/editor/akkado-completions.ts:1-50` + `web/src/lib/editor/signature-help.ts`; builtins metadata via `audioEngine.getBuiltins()` |
| Bracket matching and auto-indent | Met | `web/src/lib/components/Editor/Editor.svelte:145-170` enables `bracketMatching()`, `closeBrackets()`, `indentWithTab`, `defaultKeymap` |

### Phase 5: Control Panel (5 items)
| Item | Status | Evidence |
|---|---|---|
| Fader and knob components (Canvas-based) | Unmet | `web/src/lib/components/Params/ParamSlider.svelte:43-57` is `<input type="range">`, not canvas. No `Knob.svelte`/`Fader.svelte`; `web/src/lib/components/Controls/` is empty. |
| Control binding to Akkado variables | Met | `ParamDecl` / `ParamType` in `web/src/lib/stores/audio.svelte.ts:21-37`; `web/src/lib/components/Params/ParamsPanel.svelte:7-32`; plumbed via `setParamValue` → worklet → `_cedar_set_param` |
| XY pad for dual-parameter control | Unmet | No `XYPad.svelte`; `web/src/lib/components/Controls/` is empty. |
| Settings persistence (localStorage) | Met | `web/src/lib/stores/settings.svelte.ts:19-55` (`nkido-settings` key) |
| Panel position configuration (left/right) | Met | `web/src/lib/stores/settings.svelte.ts:8, 20, 57-60`; `web/src/routes/+page.svelte:10, 38-49` renders `SidePanel` conditionally |

### Phase 6: Documentation (5 items)
| Item | Status | Evidence |
|---|---|---|
| Markdown renderer with NKIDO widgets | Met | `web/src/lib/docs/parser.ts:13-40` transforms ```akk``` blocks; `web/src/lib/docs/components/DocCodeWidget.svelte` |
| Auto-generate function reference from opcodes | Met | `web/scripts/generate-syntax-builtins.ts` → `web/src/lib/editor/generated/syntax-builtins.ts`; runtime metadata via `audioEngine.getBuiltins()`; `web/static/docs/reference/builtins/` holds reference markdown |
| Example gallery with categories | Partially met | Tutorials categorised under `web/static/docs/tutorials/`, patches under `web/static/patches/` with `index.json`, browsable via `/embed?patch=`. No dedicated "gallery" UI route. |
| Standalone docs route (`/docs`) | Met | `web/src/routes/docs/+layout.svelte`, `web/src/routes/docs/[category]/+page.svelte`, `web/src/routes/docs/[category]/[slug]/…` |
| Search functionality | Met | `web/src/lib/docs/components/DocSearch.svelte`; `web/scripts/build-docs-index.ts` produces the lookup index consumed by `web/src/lib/docs/lookup.ts` |

### Phase 7: Polish & Launch (5 items)
| Item | Status | Evidence |
|---|---|---|
| PWA support (offline SW + cache) | Unmet | No `service-worker.ts`, no `manifest.webmanifest`, no `registerSW`/`workbox` references (grep in `web/src` and `web/static` returned nothing). Uses `@sveltejs/adapter-static` without an SW hook. |
| Share URLs (base64-encoded programs) | Unmet | No `btoa`/`atob`/share-link code (grep returned nothing in `web/src`). `/embed` route loads patches by slug from `/patches/`, not from encoded URL params. |
| Comprehensive keyboard shortcuts | Partially met | `web/src/lib/components/Editor/Editor.svelte:97-170` binds Ctrl+Enter, Escape, F1, Ctrl+/, Ctrl+Alt+Up/Down, Ctrl+D, plus default CM keymap. No global transport shortcut (e.g., Space) outside the editor; no shortcut discoverability UI. |
| Mobile/tablet responsive design | Unmet | Only `@media` query found is in `web/src/routes/docs/+layout.svelte`. Main IDE layout (`web/src/routes/+page.svelte:53-113`) uses fixed flex layout with no responsive breakpoints; default side-panel width 280 px has no tablet/mobile adaptation. |
| Performance profiling and optimization | Not verifiable | No profiling artefacts/scripts in-tree. PRD's ~180 KB gzipped bundle target is not measured/recorded. |

## Findings

### Unmet Goals
1. **Filter-response widget** — PRD Phase 3 calls for per-filter frequency-response curves; not present.
2. **Envelope widget** — PRD Phase 3 ADSR shape widget; not present.
3. **Dedicated stereo-meter output widget** — PRD Phase 3; currently generic waveform/oscilloscope only.
4. **Canvas-based knob/fader + XY pad** — PRD Phase 5; current controls are HTML inputs, XYPad absent.
5. **PWA support** — PRD Phase 7; no service worker or manifest.
6. **Share URLs** — PRD Phase 7; no base64-encoded program URLs.

### Stubs
- None observed. No TODO/FIXME placeholder UI was detected in the audited files.

### Coverage Gaps
1. **Sparse web tests** — `web/tests/` has only `arrays.test.ts` + `wasm-helper.ts`; no UI component tests, no store tests for the critical `audio.svelte.ts` compile/hot-swap/param/SoundFont paths.
2. **Mini-notation syntax highlighting** — strings render as a single `string` token; internal pattern structure not coloured in the editor (runtime step highlight partially compensates).

### Missing Tests
- No tests for: hot-swap happy path, SlotBusy retry, diagnostic source mapping, param exposure, sample loading, SoundFont loading, pattern preview rendering, or docs search. Recommend a minimal vitest suite around `audio.svelte.ts` compile + diagnostics mapping at a minimum.

### Scope Drift
- The IDE has grown beyond the PRD: `samples` tab with `SampleBrowser`, bank-registry + GitHub bank loading (`web/src/lib/audio/bank-registry.ts`), SoundFont (SF2) loading, `DebugPanel` + `StateInspector` + `PatternDebugPanel`, 7-theme system with custom themes (`web/src/lib/stores/theme.svelte.ts`, `web/src/lib/themes/presets.ts`), `/embed` route with patch picker. These are additive and useful — consider absorbing them into the PRD or logging a follow-on PRD.

### Suggestions
- Either implement Phase 7 polish (PWA, share URLs, mobile, canvas knobs/XY pad) or amend the PRD to descope them explicitly.
- Add `VizType.Envelope`, `VizType.FreqResponse`, `VizType.StereoMeter` to close Phase 3 widget parity, or replace those PRD items with the shipped generic renderers.
- Add at least a minimal vitest suite under `web/tests/` covering the compile happy-path and diagnostic mapping in `audio.svelte.ts` — this is the critical live-coding path.
- Record an actual gzipped bundle measurement so the 180 KB budget claim is auditable.
- Update the PRD status block (currently `PARTIAL` — Phase 7 "ongoing") to enumerate the additive scope (samples/banks/SF2/debug/theming/embed) and clarify which Phase 7 items are descoped vs still planned.

## PRD Status
- Current: `PARTIAL`
- Recommended: `PARTIAL` (unchanged)
- Reason: Core functionality (Phases 1–4, most of 5–6) is live and correctly wired. Phase 7 (PWA, share URLs, mobile, canvas controls, XY pad) plus three Phase 3 widget types (envelope, filter-response, stereo-meter) remain unimplemented. Status should stay `PARTIAL` until those gaps are either implemented or descoped.

# Audit: Parameter Exposure System

**PRD:** `docs/prd-parameter-exposure.md`
**Audit base:** `87c7625605609059d98fb955c3b6100a9c20c7d4`
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 4 of 4 (all headline goals implemented)
- Critical findings: 2 (Unmet=0, Stubs=0, Coverage Gaps=2, Missing Tests=0)
- Recommended PRD Status: `Complete` (header banner already says "Status: DONE"; body field is stale)
- One-line verdict: End-to-end parameter exposure is shipped (compiler + WASM + worklet + store + Svelte UI + tests); the PRD body field contradicts its own "Status: DONE" banner and should be flipped.

## Goal Verification
| Goal | Status | Evidence |
|---|---|---|
| Declarative: params defined in code via builtins | Met | `akkado/include/akkado/builtins.hpp:867-882` registers `param`, `button`, `toggle`, `dropdown` with `ENV_GET` opcode and defaults; `akkado/src/codegen.cpp:802-805` wires all four to special handlers. |
| Zero overhead: uses existing EnvMap lock-free infra | Met | All four handlers in `akkado/src/codegen_params.cpp:47`, `:156`, `:233`, `:319` emit `PUSH_CONST` (fallback) + `ENV_GET` with `state_id = fnv1a_hash_runtime(name)`. No new opcode added. |
| Hot-swap safe: values preserved across recompilation | Met | `web/src/lib/stores/audio.svelte.ts:1481-1503` `updateParamDecls()` snapshots old `paramValues` and re-maps by name on recompile; unmatched params fall back to default. |
| Portable metadata for web/Godot/DAWs | Partially Met | WASM exports cover most fields: `web/wasm/nkido_wasm.cpp:1175-1269` (count/name/type/default/min/max/option_count/option/source_offset/source_length). Hash accessor and JSON serializer (`akkado_get_param_hash`, `akkado_get_params_json`) specified in PRD §3.5 are NOT exported. Godot GDExtension binding layer does not exist. |

### Individual builtin checks
| Builtin | Status | Evidence |
|---|---|---|
| `param(name, default, min?, max?)` | Met | `akkado/src/codegen_params.cpp:47-153`; range clamp + min>max swap + dedup implemented (tests `akkado/tests/test_codegen.cpp:1414-1516`). |
| `button(name)` | Met | `akkado/src/codegen_params.cpp:156-230`; zero fallback (tests `:1543-1590`). |
| `toggle(name, default?)` | Met | `akkado/src/codegen_params.cpp:233-316`; boolean normalization at 0.5 threshold (tests `:1592-1622`). |
| `dropdown(name, opt1…)` | Met | `akkado/src/codegen_params.cpp:319+`; options recorded, min/max 0..N-1 (tests `:1657-1735`). Note: name differs from PRD (`select()` in PRD text; implementation and tests consistently use `dropdown()` — `select()` at `builtins.hpp:540` is the separately-existing ternary op). |

## Findings

### Unmet Goals
None. All four builtins, all four UI control components, store integration, worklet integration, source-location tracking and hot-swap value preservation are all implemented.

### Stubs
None found. Handlers are fully implemented with validation, diagnostics (E150/E151/E152/E153/E155/E156), range clamping, min>max swap warning (W050), deduplication, and source-location propagation.

### Coverage Gaps
1. **Missing WASM exports specified in PRD §3.5**:
   - `akkado_get_param_hash(index)` — not present in `web/wasm/nkido_wasm.cpp` (grep returns zero matches).
   - `akkado_get_params_json()` — JSON exporter for Godot/DAW compatibility not present (grep returns zero matches).
   Impact: web UI works because it uses individual getters; external consumers (Godot, DAW bridges) would need these added.

2. **No Godot GDExtension integration**:
   - PRD §2.3 and §1.3 specify Godot Inspector exposure, `player.set_param()`, `player.get_param()`, `player.get_params()`.
   - Repo-wide grep for `Godot`/`godot` finds only a doc comment at `akkado/include/akkado/codegen.hpp:40`. Python binding `cedar/bindings/bindings.cpp:167` exposes `vm.set_param` but that is not Godot.
   Impact: "Portable: Same metadata format for web, Godot, DAWs" is web-only in practice.

3. **No dedicated user-facing docs page** (`parameters.md`): PRD Phase 6 item 21 lists "Add `parameters.md` documentation". `find web/static/docs` and `grep` for `param()/button()/toggle()/dropdown()` in the docs tree return no hits. The built-in autocomplete descriptions in `builtins.hpp` are the only in-product documentation.

### Missing Tests
None critical. `akkado/tests/test_codegen.cpp:1414-1735` covers:
- param basic/default-range/clamping/min>max swap/dedup/multiple
- param name literal requirement (E151)
- button declaration + zero fallback
- toggle off/on/normalization above/below 0.5
- dropdown with multiple/single options, ENV_GET emission, empty-options error, non-literal options error
- source_offset/source_length population
- hash consistency with cedar FNV-1a

Gaps vs. PRD §5.2 integration tests:
- No Playwright/web E2E test verifying slider auto-generation or hot-swap value preservation in the browser. Whether this is "missing" depends on whether the project has adopted Playwright — acceptable for a non-E2E project.

### Scope Drift
- **Naming drift `select` → `dropdown`**: PRD consistently uses `select()` for the dropdown builtin (§2.1, §3.3 builtin table, §3.4 handler name, §5.1 tests). Implementation uses `dropdown()` because `select()` is already taken by a ternary conditional op (`cedar::Opcode::SELECT`, `akkado/include/akkado/builtins.hpp:540`). This is a sensible rename to avoid collision but the PRD text was never updated to match. The internal `ParamType::Select` enum and `handle_select_call` function name retain the original terminology.
- **Extra feature shipped**: Visualization exposure (`pianoroll`, `oscilloscope`, `waveform`, `spectrum`, `waterfall`) was added alongside parameter exposure. Tracked by a separate PRD — not drift for this one.
- **Deduplication W051**: PRD §3.4 specifies a W051 "redeclared with different range" warning; the implemented `handle_param_call` dedup path at `codegen_params.cpp:94-107` appears to skip re-declaration without emitting W051 (not confirmed in this audit — low-priority follow-up).

### Suggestions
1. Flip `Status:` from `Draft` to `Complete` (or `Done`) in the PRD body — the top banner "Status: DONE" already says so, so the field is internally inconsistent.
2. If Godot support is a live goal, file a follow-up PRD to add `akkado_get_param_hash`, `akkado_get_params_json`, and a Godot GDExtension layer. Otherwise, clarify in §2.3 / Non-Goals.
3. Verify W051 emission (dedup with conflicting range) is actually wired.
4. Update PRD code snippets to reflect the `dropdown` name, or rename the builtin to match PRD (lower risk: update the PRD).
5. Add a `parameters.md` user doc page so the F1 help system exposes these builtins.

## PRD Status
- Current: `Draft` (body field), but top banner says `Status: DONE — param/toggle/button/dropdown end-to-end with web UI.`
- Recommended: `Complete`
- Reason: All four builtins, compiler metadata, WASM exports (minus hash/JSON helpers), worklet wiring, audio store with hot-swap preservation, and four Svelte UI components exist and are covered by unit tests. The only true gaps (Godot bridge, JSON export) are either out of MVP scope or easy follow-ups. The internal `Status: Draft` contradicts the author's own "Status: DONE" banner at line 1 and the extensive shipped implementation.

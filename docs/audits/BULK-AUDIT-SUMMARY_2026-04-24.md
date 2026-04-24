# Bulk PRD Audit Summary — 2026-04-24

Read-only audit of every PRD with TODO / in-progress / draft / unchecked-box signals. 22 PRDs audited in parallel by independent subagents; each produced a per-PRD report at `docs/audits/<stem>_audit_2026-04-24.md`.

No source code was modified, no tests were run, no PRD Status lines were edited.

## Headline numbers

- **22 PRDs audited**
- **3 genuinely complete** (code matches what the PRD promised)
- **3 shipped in external sibling repos** (`prd-cedar-esp32` → `~/workspace/cedar-esp32`, `prd-godot-extension` → `~/workspace/godot-nkido-addon`, `prd-project-website` → `~/workspace/nkido.cc`) — their Status lines have been updated to `SHIPPED (external)` and their per-PRD audit reports removed
- **8 mostly complete** (code shipped, gaps are docs / tests / minor goals)
- **6 partially done** (substantial features missing or scope diverged)
- **1 not started** (`prd-audio-input`)
- **1 superseded** (1st-draft Godot PRD)

## By recommended status

### Complete — no action needed on code
| PRD | Goals | Notes |
|---|---|---|
| `prd-language-extensions` | 8/8 | Feature 5 uses E133 in codegen instead of `param_types` annotation (minor divergence). |
| `prd-parameter-exposure` | 4/4 | PRD body says "Draft" but banner says DONE; implementation fully shipped. Dropdown builtin named `dropdown()` not `select()` (collision avoidance). |
| `prd-tap-delay` | 14/17 | Shipped with bonus dry/wet and delay-time smoothing; PRD checkboxes stale. |

### Mostly complete — code shipped, docs or tests trailing
| PRD | Goals | Gap |
|---|---|---|
| `prd-advanced-functions` | 6/6 features | Phase 6 docs skipped; `docs/agent-guide-userspace-functions.md` claims "numeric-only defaults" (stale). |
| `prd-conditionals-logic` | 4/4 | Phase 5 docs + F1 help entries absent; tests are codegen-presence only, no value assertions; no epsilon-equality test. |
| `prd-underscore-placeholder` | 6/8 | E107 named-arg rejection unimplemented **and** collides with existing E107 "Unknown function" at `codegen.cpp:917`. |
| `prd-timeline-curve-notation` | 33/34 | User-facing docs for `t"..."` and curve atoms absent; no W200/empty-curve test. |
| `prd-editor-autocomplete` | 4/4 + 7/10 | `web/tests/` has only `arrays.test.ts` — no autocomplete tests. |
| `prd-records-and-field-access` | 4/5 | Extended pattern fields (dur/chance/time/phase/voice/note/sample/sample_id) unimplemented; record spreading shipped despite being deferred in PRD. |
| `prd-visualization-system` | 5/5 | Partially superseded by viz-revision (additive). Unchecked boxes don't match DONE banner. |
| `prd-microtonal-extension` | 12/17 | `d` / `\` aliases and `^n` numeric form unimplemented; no JI/BP presets; stale `is_accidental()` helper. |

### Partially done — substantial gaps
| PRD | Goals | Gap |
|---|---|---|
| `prd-file-loading-abstraction` | 3/5 + 1 partial | SF2/SF3 loader, `load_mapped`/mmap, and TS cache-management UI (`isCached`/`clearCache`/`getCacheStats`) absent. PRD has conflicting `Status: DONE` (line 1) and `Status: Draft` (line 5). |
| `prd-soundfont-playback-fixes` | 3/4 | Both C++ bug fixes shipped (commit `9e4186e`), but `experiments/test_op_soundfont.py`, cedar_core SF bindings, and Phase 2 dedup unit test absent. PRD still reads NOT STARTED. |
| `prd-soundfonts-sample-banks` | 4/5 + scope divergence | TinySoundFont in C++ instead of PRD's SpessaSynth-in-TS; single `SOUNDFONT_VOICE` opcode vs three; no BankList / AddBankDialog UI; no default TR808/909/linndrum registration; zero SoundFont tests. |
| `prd-pattern-array-note-extensions` | 4/8 | Phase 7 modifiers and Phase 8 `run`/`binary`/`binaryN` not started; voicing deferred; bend/aftertouch missing. |
| `prd-nkido-web-ide` | 5/5 top-level, ~30/37 checklist | Filter-response / envelope / stereo-meter widgets, canvas knob/fader + XY pad, PWA, share URLs absent. |
| `prd-viz-system-revision` | ~47/48 checkboxes (Rev 1) | Rev 1 complete, Rev 2 partial (waterfall lacks ResizeObserver), Rev 3 not started. |

### Shipped in external sibling repos
| PRD | Sibling repo | Notes |
|---|---|---|
| `prd-cedar-esp32` | `~/workspace/cedar-esp32` | Implementation lives in the sibling repo. PRD Status flipped to `SHIPPED (external)`; per-PRD audit removed. |
| `prd-godot-extension` | `~/workspace/godot-nkido-addon` | Implementation lives in the sibling repo. PRD Status flipped to `SHIPPED (external)`; per-PRD audit removed. |
| `prd-project-website` | `~/workspace/nkido.cc` | Implementation lives in the sibling repo. PRD Status flipped to `SHIPPED (external)`; per-PRD audit removed. |

### Not started — correctly labeled
| PRD | Notes |
|---|---|
| `prd-audio-input` | 0/7. Every Modify file unchanged, every Create file absent. PRD dated 2026-04-21 — 3 days stale. |

### Superseded — archive candidate
| PRD | Action |
|---|---|
| `prd-godot-extension-1st-draft` | Renamed to `-1st-draft` in commit `daddb4e` when `prd-godot-extension.md` replaced it. Flip `Status: Draft` → `Archived (superseded)` and consider moving to `docs/archived/`. |

## Cross-cutting patterns

1. **Stale Status lines are the norm, not the exception.** Multiple PRDs have banners saying `DONE` while the body says `Draft` (or vice versa), and several with `NOT STARTED` headers describe already-shipped features. The PRD Status field is unreliable as a signal — the `grep Status` scan at the top of this session only identified ~7 PRDs with explicit Status, and several of those were wrong.

2. **Docs are the most common trailing edge.** Code ships; F1 help, reference docs, and user-facing markdown lag. `prd-advanced-functions`, `prd-conditionals-logic`, `prd-timeline-curve-notation`, `prd-microtonal-extension` all have "Phase N: docs" as the unmet phase.

3. **Tests passing ≠ coverage.** Several PRDs have green Catch2 suites that only verify opcode emission, not runtime behavior. `prd-conditionals-logic` is the clearest example: the existing tests would pass even if `==` / `!=` / `select` inverted their polarity.

4. **Scope divergence in the soundfont/sample stack.** Three PRDs (`prd-file-loading-abstraction`, `prd-soundfont-playback-fixes`, `prd-soundfonts-sample-banks`) all touch the same stack and have diverged from their original specs (TinySoundFont in C++ instead of SpessaSynth in TS; a single opcode instead of three). Worth a consolidation review.

5. **External-repo PRDs are unauditable from inside nkido.** `prd-cedar-esp32` and `prd-godot-extension` scope work to sibling directories that aren't in this tree. Consider adding a pointer doc or submodule when work begins externally.

## Recommended next actions

1. **Low-effort wins:** flip Status lines on the 3 Complete + 8 Mostly Complete PRDs to reflect reality. The `prd-godot-extension-1st-draft` rename to Archived is a one-line edit.
2. **Fix the E107 collision** in `prd-underscore-placeholder` — it's a real bug, not just a doc gap.
3. **Write missing tests or docs** for the Mostly Complete bucket in priority order: `prd-conditionals-logic` (runtime value assertions), `prd-timeline-curve-notation` (user-facing docs), `prd-editor-autocomplete` (web test harness).
4. **Run the full `/prd-implementation-audit`** skill interactively on any of the Partially Done PRDs where you want test additions and Status edits applied. This bulk scan stopped at the report stage.

## Report index

All per-PRD reports live at `docs/audits/<stem>_audit_2026-04-24.md`. Full list:

- prd-advanced-functions_audit_2026-04-24.md
- prd-audio-input_audit_2026-04-24.md
- prd-conditionals-logic_audit_2026-04-24.md
- prd-editor-autocomplete_audit_2026-04-24.md
- prd-file-loading-abstraction_audit_2026-04-24.md
- prd-godot-extension-1st-draft_audit_2026-04-24.md
- prd-language-extensions_audit_2026-04-24.md
- prd-microtonal-extension_audit_2026-04-24.md
- prd-nkido-web-ide_audit_2026-04-24.md
- prd-parameter-exposure_audit_2026-04-24.md
- prd-pattern-array-note-extensions_audit_2026-04-24.md
- prd-records-and-field-access_audit_2026-04-24.md
- prd-soundfont-playback-fixes_audit_2026-04-24.md
- prd-soundfonts-sample-banks_audit_2026-04-24.md
- prd-tap-delay_audit_2026-04-24.md
- prd-timeline-curve-notation_audit_2026-04-24.md
- prd-underscore-placeholder_audit_2026-04-24.md
- prd-visualization-system_audit_2026-04-24.md
- prd-viz-system-revision_audit_2026-04-24.md

**Removed from this audit pass** (shipped in external sibling repos):
- ~~prd-cedar-esp32_audit_2026-04-24.md~~ → see `~/workspace/cedar-esp32`
- ~~prd-godot-extension_audit_2026-04-24.md~~ → see `~/workspace/godot-nkido-addon`
- ~~prd-project-website_audit_2026-04-24.md~~ → see `~/workspace/nkido.cc`

## Audit mode

This was a **read-only bulk audit** — no tests run, no source edits, no PRD Status edits, no new tests written. The full `/prd-implementation-audit` skill runs all of those; running it 22× in parallel would thrash the machine, so this pass was scoped to goal tracing + stub detection + coverage identification only. For any PRD where you want the full finalize-with-tests-and-Status-edit loop, invoke `/prd-implementation-audit` on it individually.

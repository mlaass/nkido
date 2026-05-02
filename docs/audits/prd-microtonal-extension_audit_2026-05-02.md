# Audit: Akkado Microtonal Extension

**PRD:** `docs/prd-microtonal-extension.md`
**Audit base:** `12f8231` (parent of `2da7a73 Add microtonal tuning support`, 2026-02-16)
**Audit head:** `fea1d13` (working tree, includes audit-time fixes)
**Audited:** 2026-05-02

The PRD's Status line said `NOT STARTED` at the start of the audit — but the feature shipped in commit `2da7a73` on 2026-02-16, two and a half months before this audit. The PRD had simply gone stale. This audit verifies the shipped implementation against every promise in the PRD and closes the gaps that were still open.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| Pitch atom accepts modifier stream `^`, `v`, `+`, `x` | Met | `akkado/src/mini_lexer.cpp:608–637` |
| Aliases `d` and `\` (Stein-Zimmermann / alt-down) | Met (audit fix) | `akkado/src/mini_lexer.cpp:608–637` (added during audit) |
| Numeric arguments `^n` / `vn` | Descoped | PRD §3.3 updated — conflicts with octave digit syntax; stacking remains supported |
| `micro_offset` field on `MiniPitchData` | Met | `akkado/include/akkado/mini_token.hpp:122` |
| `micro_offset` field on `MiniAtomData` | Met | `akkado/include/akkado/ast.hpp:196` |
| `TuningContext` struct | Met | `akkado/include/akkado/tuning.hpp` |
| `tune()` compile-time scope modifier | Met | `codegen_patterns.cpp:4038` (handler), `:2154` (transform path) |
| Codegen Hz conversion uses tuning context | Met | `codegen_patterns.cpp:425` |
| Default 12-EDO context | Met | `TuningContext` default member values |
| Built-in EDO presets (12/17/19/22/24/31/41/53) | Met | Any `Nedo` parses via `parse_tuning()` (`akkado/src/tuning.cpp`) |
| `ji` (5-limit symmetric) preset | Met (audit fix) | `tuning.hpp` `Kind::JI`, ratio table, `resolve_hz` JI branch |
| `bp` (Bohlen-Pierce 13edt) preset | Met (audit fix) | `tuning.hpp` `Kind::BP`, `resolve_hz` BP branch |
| SoundFont fractional-MIDI fix | Met | `cedar/include/cedar/opcodes/soundfont.hpp:127, 168` (zone select still rounds at `:78`) |

## Validation Commands

| Command | Result | Notes |
|---|---|---|
| `cmake --build build --target akkado_tests cedar_tests` | Pass | Pre-existing build break in unrelated cedar URI-resolver work-in-progress was self-resolved by the user mid-audit |
| `akkado_tests "[tuning],[microtonal]"` (initial) | Pass (94 assertions / 5 cases) | Pre-existing baseline coverage |
| `akkado_tests` (full) | Pass (137554 assertions / 563 cases) | After audit additions |
| `cedar_tests` (full) | Pass (334843 assertions / 184 cases, 1 skipped) | SoundFont path not regressed |

## Findings

### Unmet Goals
- **`d` alias not implemented** (PRD §4.2 / §4.4.1). Lexer at `akkado/src/mini_lexer.cpp:608–617` recognized only `^/v/+/x`. `cd4`, `cbd4` did not parse as the spec required. **Resolved during audit** — added with a forward-lookahead disambiguation rule (only a modifier when the modifier chain reaches an octave digit) so sample names `bd`/`sd`/`cp` continue to parse as samples.
- **`\` alias not implemented** (PRD §4.2). **Resolved during audit** — added unconditionally to the modifier stream (no sample-name conflict).
- **Numeric arguments `^n` / `vn`** (PRD §3.3). **Descoped during audit** — irreconcilable with the octave-digit syntax (`c^4` already means "c with one micro step up, octave 4"). PRD §3.3 was rewritten as a descope note; stacking (`c^^^^4`) remains the supported way to express larger offsets.
- **`ji` preset** (PRD §6.1). `parse_tuning()` only accepted `Nedo`. **Resolved during audit** — added `Kind::JI` with the 5-limit symmetric 12-tone ratio table, anchored at C4.
- **`bp` preset** (PRD §6.1). **Resolved during audit** — added `Kind::BP` (13edt, tritave divisions ~146.3¢ each). MIDI is reinterpreted as a step counter along the BP scale; documented in PRD §5.2 and Roadmap.

### Stubs
None.

### Regressions
None. Both base and head pass their full suites.

### Coverage Gaps
- **No Hz round-trip test for `tune()`** — pre-audit coverage only checked compile success, so a regression in `(midi, micro) → Hz` could pass silently. **Resolved during audit** — added `tune() emits correct Hz for microtonal notes` covering 12-EDO, 24-EDO, 31-EDO, `d` alias, `\` alias.
- **PRD §4.3 stacking table not exhaustively covered** — only `c^4`, `c#^4`, `cvv4`, `c+4`, `c#^v4` had explicit tests. **Resolved during audit** — `PRD §4.3 stacking table` test covers every row including `cbb4`, `cbd4`, `cd4`, `cx4`, `c\4`.
- **`d` alias regression risk against sample names** — fix has dedicated test `d alias does not break sample tokens` (covers `bd`, `sd`, `bd:2`, standalone `d`, `d4`).
- **No tests for `ji` / `bp`** — pre-audit, both presets didn't exist. **Resolved during audit** — `parse_tuning recognizes ji and bp presets`, `JI tuning resolves ratios from 5-limit symmetric scale`, `BP tuning uses 13 equal divisions of the tritave`, plus end-to-end compile tests.

### Missing Tests
None unresolved (PRD did not name specific test artifacts beyond what is now covered).

### Scope Drift
- The original implementation commit `2da7a73` also touched `akkado/include/akkado/pattern_event.hpp`, `akkado/src/pattern_eval.cpp`, `akkado/src/pattern_debug.cpp`, and `akkado/src/mini_parser.cpp`. These are not listed in PRD §7's file-level scope but are required to thread `micro_offset` from the lexer through pattern evaluation to codegen. Documented as expected-but-undocumented; PRD §4.4 was updated to mention `pattern_event.hpp`.

### Convention Drift
None observed.

### Suggestions
- BP currently reuses 12-EDO MIDI as a step counter, which is functional but unintuitive. A dedicated note-nominal scheme for BP (with 13 step letters) would be more usable; logged as a Future item.
- JI is currently anchored at C with no way to change the tonic. Configurable tonic + prime-limit ratios are logged in Future.

## Decisions Recorded

- User chose **Implement now** for `d`, `\`, JI, and BP. Implemented during this audit.
- User chose **Mark as deprecated/descoped** for numeric-argument syntax (`^n` / `vn`) — confirmed conflict with octave digit. PRD §3.3 rewritten to capture the rationale; stacking (`^^^^`) remains the supported form.
- User chose **Add full §4.3 table coverage** and **Add Hz round-trip test**.
- Disambiguation rule for `d` (forward-lookahead requiring an octave digit somewhere in the chain) was a judgment call needed to keep `bd`/`sd` sample names intact. PRD §4.2 was tightened to describe the rule; tests pin both directions.
- A pre-existing build break in unrelated cedar URI-resolver work-in-progress (`cedar/src/io/file_loader.cpp` missing `<filesystem>` include after the header lost its transitive include) self-resolved — likely a parallel-process touch — and the audit proceeded without modifying URI-resolver code.

## Tests Added / Extended

| File | Kind | Covers |
|---|---|---|
| `akkado/tests/test_mini_notation.cpp` `tune() emits correct Hz for microtonal notes` | new | 12-EDO `c^4 == c#4`, 24-EDO quarter tone, 31-EDO ~38.7¢ step, `cbd4` sesquiflat, `c\4` alt-down — emitted Hz validated, not just compile success |
| `akkado/tests/test_mini_notation.cpp` `PRD §4.3 stacking table` | new | `c#4`, `c^4`, `c#^4`, `cbb4`, `cbd4`, `c+4`, `cd4`, `cx4`, `c\4` — every row in §4.3 plus the new aliases |
| `akkado/tests/test_mini_notation.cpp` `d alias does not break sample tokens` | new | Regression guard: `bd`, `sd`, `bd:2`, standalone `d`, `d4` |
| `akkado/tests/test_mini_notation.cpp` `parse_tuning recognizes ji and bp presets` | new | New presets are parseable |
| `akkado/tests/test_mini_notation.cpp` `JI tuning resolves ratios from 5-limit symmetric scale` | new | C4 anchor, E4 = 5/4, G4 = 3/2, C5 = 2/1, micro_offset traversal |
| `akkado/tests/test_mini_notation.cpp` `BP tuning uses 13 equal divisions of the tritave` | new | Step size, +1 step ratio, 13 steps = 3:1 |
| `akkado/tests/test_mini_notation.cpp` `tune("ji") and tune("bp") compile end-to-end` | new | New presets compile in full pipeline |
| `akkado/tests/test_mini_notation.cpp` (existing `parse_tuning` SECTION) | extended | `edo_count` field renamed to `divisions` to match new `TuningContext` shape |

## Post-Finalize Validation

| Command | Result |
|---|---|
| `cmake --build build --target akkado_tests` | Built clean |
| `cmake --build build --target cedar_tests` | Built clean |
| `akkado_tests "[tuning],[microtonal],[ji],[bp],[ji_bp],[hz_roundtrip],[prd_4_3],[regression]"` | Pass (193 assertions / 13 cases) |
| `akkado_tests` (full) | Pass (137554 assertions / 563 cases) — ~+94 assertions vs. base |
| `cedar_tests` (full) | Pass (334843 assertions / 184 cases, 1 skipped) |

## PRD Status

- Before: `NOT STARTED` (stale by ~2.5 months)
- After: `COMPLETE`

## Recommended Next Steps

None blocking. Future work captured in PRD §7 Future:
- Scala/TUN file loading.
- Configurable JI tonic + prime-limit.
- Microtonal chord voicings.
- Runtime `detune()`.
- Dedicated BP note-nominal scheme.

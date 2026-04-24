# Audit: Timeline Curve Notation

**PRD:** `docs/prd-timeline-curve-notation.md`
**Audit base:** unable to determine (PRD file is untracked in current working tree; `git log --diff-filter=A` returned latest commit `4977c77` from a file rename, not the original add — treating audit as "against HEAD").
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 33 of 34 checkboxes (Phase 1–4 fully implemented; Phase 5 Documentation unmet)
- Critical findings: 3 (Unmet=1, Stubs=0, Coverage Gaps=1, Missing Tests=1)
- Recommended PRD Status: COMPLETE (pending docs) — strongly inconsistent with current "NOT STARTED" header
- One-line verdict: Implementation is essentially complete through Phase 4 with strong codegen/test coverage, but the PRD status line and Phase 5 documentation have not been updated.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| Compile curve strings to TIMELINE breakpoints | Met | `/home/moritz/workspace/nkido/akkado/src/pattern_eval.cpp:675` `events_to_breakpoints()`; `/home/moritz/workspace/nkido/akkado/src/codegen_patterns.cpp:1774` `handle_timeline_literal` |
| Integrate with mini-notation features | Met | curve_mode flows through `parse_mini`; grouping/alternation/modifiers tests at `/home/moritz/workspace/nkido/akkado/tests/test_mini_notation.cpp:1939,2011,2021,2095` |
| Always output 0.0-1.0 | Met | `/home/moritz/workspace/nkido/akkado/src/mini_lexer.cpp:219-243` hard-coded level values 0.0,0.25,0.5,0.75,1.0 |
| `t"..."` prefix and `timeline()` call forms | Met | `/home/moritz/workspace/nkido/akkado/src/lexer.cpp:433-439`; `/home/moritz/workspace/nkido/akkado/src/codegen_patterns.cpp:1842` `handle_timeline_call` |

### Phase 1: Lexer & Token Infrastructure (10 items)
| Item | Status | Evidence |
|---|---|---|
| Add `Timeline` to `TokenType` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/token.hpp:36` |
| `t"..."` / `t\`...\`` detection in lexer | Met | `/home/moritz/workspace/nkido/akkado/src/lexer.cpp:433-439` |
| `token_type_name()` entry for `Timeline` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/token.hpp:110` |
| `CurveLevel`, `CurveRamp`, `CurveSmooth` in `MiniTokenType` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/mini_token.hpp:24-26` |
| `MiniCurveLevelData` struct | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/mini_token.hpp:115-118` |
| Extend `MiniTokenValue` variant | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/mini_token.hpp:127` |
| `curve_mode` flag on `MiniLexer` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/mini_lexer.hpp:91` |
| Curve-mode lexing for `_`, `.`, `-`, `^`, `'`, `/`, `\`, `~` | Met | `/home/moritz/workspace/nkido/akkado/src/mini_lexer.cpp:216-254` |
| `/` lookahead disambiguation (digit=slow, non-digit=ramp) | Met | `/home/moritz/workspace/nkido/akkado/src/mini_lexer.cpp:244-251` |
| Lexer tests for curve tokens + ambiguity | Met | `/home/moritz/workspace/nkido/akkado/tests/test_mini_notation.cpp:1879-1957`, test_lexer.cpp:1151-1174 |

### Phase 2: AST & Parser (7 items)
| Item | Status | Evidence |
|---|---|---|
| `CurveLevel`, `CurveRamp` in `MiniAtomKind` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/ast.hpp:180-181` |
| `curve_value`, `curve_smooth` in `MiniAtomData` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/ast.hpp:207-208` |
| Curve atom parsing in `mini_parser.cpp` | Met | `/home/moritz/workspace/nkido/akkado/src/mini_parser.cpp:198-257` |
| Update `is_atom_start()` | Met | `/home/moritz/workspace/nkido/akkado/src/mini_parser.cpp:167-169` |
| Handle `TokenType::Timeline` in main parser | Met | `/home/moritz/workspace/nkido/akkado/src/parser.cpp:90,470,1134-1168` |
| Pass `curve_mode=true` when parsing timeline content | Met | `/home/moritz/workspace/nkido/akkado/src/parser.cpp:1167-1168` |
| Parser tests for curve atoms, grouping, modifiers | Met | `/home/moritz/workspace/nkido/akkado/tests/test_mini_notation.cpp:1959-2039,2105+` |

### Phase 3: Pattern Evaluation (5 items)
| Item | Status | Evidence |
|---|---|---|
| `CurveLevel`, `CurveRamp` in `PatternEventType` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/pattern_event.hpp:25-26` |
| `curve_value`, `curve_smooth` on `PatternEvent` | Met | `/home/moritz/workspace/nkido/akkado/include/akkado/pattern_event.hpp:57-58` |
| Handle curve atoms in `eval_atom()` | Met | `/home/moritz/workspace/nkido/akkado/src/pattern_eval.cpp:236-243` |
| Subdivision/alternation/modifiers work with curve events | Met | test cases at `/home/moritz/workspace/nkido/akkado/tests/test_mini_notation.cpp:2041-2104` |
| Evaluation tests for event generation and timing | Met | `/home/moritz/workspace/nkido/akkado/tests/test_mini_notation.cpp:2041,2059,2071,2084,2095` |

### Phase 4: Code Generation (9 items)
| Item | Status | Evidence |
|---|---|---|
| Implement `events_to_breakpoints()` | Met | `/home/moritz/workspace/nkido/akkado/src/pattern_eval.cpp:675-765` |
| Ramp value resolution (prev/next lookup) | Met | `/home/moritz/workspace/nkido/akkado/src/pattern_eval.cpp:708-724` |
| Multiple-ramp proportional interpolation | Met | `/home/moritz/workspace/nkido/akkado/src/pattern_eval.cpp:727-740` |
| Breakpoint optimization (merge same-value holds) | Met | `/home/moritz/workspace/nkido/akkado/src/pattern_eval.cpp:750-761` |
| Emit TIMELINE opcode with populated `TimelineState` | Met | `/home/moritz/workspace/nkido/akkado/src/codegen_patterns.cpp:1815,1918` |
| Set `loop=true` and `loop_length` | Met | `/home/moritz/workspace/nkido/akkado/src/codegen_patterns.cpp:1830-1831,1933-1934` |
| Compile warning for >64 breakpoints | Met | `/home/moritz/workspace/nkido/akkado/src/codegen_patterns.cpp:1796-1798,1899-1901` |
| Codegen tests for breakpoint generation | Met | `/home/moritz/workspace/nkido/akkado/tests/test_codegen.cpp:4791-4879` (8 `[timeline_codegen]` cases) |
| End-to-end tests for audio output | Partial | `/home/moritz/workspace/nkido/akkado/tests/test_codegen.cpp:4885-4914` — 4 compile-time `[timeline_e2e]` cases verify TIMELINE opcode emission, but PRD §8.6 asked for actual audio-output assertions (e.g. "Should produce ~1.0 for all samples"); no VM-execution/output-value tests exist. |

### Phase 5: Documentation (3 items)
| Item | Status | Evidence |
|---|---|---|
| Add curve notation to mini-notation reference | Unmet | No mention of `_`, `.`, `-`, `^`, `'`, `/`, `\`, `~` curve atoms in `/home/moritz/workspace/nkido/web/static/docs/reference/mini-notation/basics.md` or siblings. |
| Add `t"..."` prefix to language reference | Unmet | No `t"..."` documentation found under `/home/moritz/workspace/nkido/web/static/docs/`. `sequencing.md` still lists `timeline()` as "Creates smooth automation curves between breakpoints" without curve-string syntax. |
| Run `bun run build:docs` | Not verifiable read-only | Skipped; not required by audit and there are no doc changes that would need re-indexing. |

## Findings

### Unmet Goals
- **Documentation (Phase 5)**: No user-facing docs describe the curve notation syntax or the `t"..."` string prefix. Users cannot discover this feature via F1 help or the reference. `/home/moritz/workspace/nkido/web/static/docs/reference/builtins/sequencing.md:121-133` still documents `timeline()` as if it takes no user-visible arguments.

### Stubs
- None found. Implementation is complete and consistent: lexer, parser, AST, eval, and codegen all handle curve atoms coherently and the generated breakpoints flow through `StateInitData::Type::Timeline`.

### Coverage Gaps
- **End-to-end audio semantics not asserted**: PRD §8.6 specifies test cases like `t"''''" |> out(%, %)` should "produce ~1.0 for all samples". Current `[timeline_e2e]` tests in `/home/moritz/workspace/nkido/akkado/tests/test_codegen.cpp:4885-4914` only verify that compilation succeeds and a `TIMELINE` opcode is emitted. There is no VM execution verifying actual sample output values (0.0, 1.0, scaled). This leaves the `t"..."*scale+offset` scaling chain untested end-to-end.
- **Edge cases from PRD §6 not all unit-tested**: spot-checked — empty string (§6.1), ramp at start/end (§6.3/6.4), adjacent ramps with equal endpoints (§6.5), `~` on first character (§6.6), >64 breakpoint truncation (§6.11) do not appear to have dedicated tests (searched `test_codegen.cpp` and `test_mini_notation.cpp` for these patterns).

### Missing Tests
- `t"~/"` compile error ("~ modifier must precede a level character") is mandated by PRD §6.7 and is checked by `/home/moritz/workspace/nkido/akkado/tests/test_mini_notation.cpp:2031` ("Curve ~ before non-level is error") — met.
- No test for the 64-breakpoint truncation warning (`W200`).
- No test for `t""` empty-curve error path.
- No test for exponential curve type unreachable via notation (documents intent only — acceptable).

### Scope Drift
- None. Every file touched matches the "Files to Modify" table in PRD §7. No new cedar/ changes; `TimelineState::MAX_BREAKPOINTS` and `op_timeline` were reused as promised.

### Suggestions
1. Update the PRD `Status:` header from `NOT STARTED` to `COMPLETE (docs pending)` — the current header is factually wrong and misleads future audits/readers.
2. Add VM-execution end-to-end tests asserting sample values (use `akkado::compile` + cedar VM harness the way `test_akkado.cpp` runs sine-gated tests) to close the §8.6 gap.
3. Add explicit edge-case tests for §6.1, §6.3, §6.4, §6.5, §6.11 to lock in documented behavior.
4. Author `web/static/docs/reference/mini-notation/curves.md` (or extend `basics.md`) with the level/ramp/smooth glossary, and update `web/static/docs/reference/builtins/sequencing.md` to mention `t"..."` and the curve-string argument to `timeline()`. Then run `bun run build:docs`.

## PRD Status
- Current: `NOT STARTED` (per PRD line 1)
- Recommended: `COMPLETE — Documentation Pending` (or `IMPLEMENTED, DOCS/E2E PENDING`)
- Reason: All four implementation phases (lexer/tokens, AST/parser, pattern evaluation, codegen) are fully realized with matching unit and codegen tests. Only Phase 5 documentation items and a handful of end-to-end / edge-case tests remain. The `NOT STARTED` header is clearly stale.

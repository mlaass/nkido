# Audit: Underscore Placeholder PRD — Positional Default Skipping

**PRD:** `docs/prd-underscore-placeholder.md`
**Audit base:** `4977c77217d47b5a9eb1c1c5c37c6cb77e7b3625`
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 6 of 8
- Critical findings: 3 (Unmet=1, Stubs=0, Coverage Gaps=1, Missing Tests=1)
- Recommended PRD Status: Mostly Done — tests incomplete, E107 collision
- One-line verdict: Core feature (default-filling + partial-application disambiguation) is fully implemented in parser/analyzer/codegen, but the PRD's E107 error code collides with the existing "Unknown function" error and codegen tests for the underscore feature are missing.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| `_` in builtin call fills default | Met | `akkado/src/codegen.cpp:942-971` detects `Identifier("_")` and emits `PUSH_CONST` with `builtin->get_default(arg_idx)` |
| `_` on required builtin param → E106 | Met | `akkado/src/codegen.cpp:967-969` emits `error("E106", "Cannot skip required parameter '..." + " — no default value")` |
| `_` in user-fn call fills default | Met | `akkado/src/codegen_functions.cpp:183-241` handles numeric, const-expression, and string defaults via same sentinel detection |
| `_` on required user-fn param → E106 | Met | `akkado/src/codegen_functions.cpp:236-240` |
| `_` in named argument → E107 | **Unmet** | No code path emits E107 for named-arg placeholders. `parse_argument` (`parser.cpp:1097-1118`) wraps values into `Argument` nodes with optional name, and codegen does not check whether the wrapping `Argument` has a name before substituting defaults. Additionally, E107 is already defined as "Unknown function" in `codegen.cpp:917` and `docs/prd-error-handling-recovery.md:280`, creating an error-code collision. |
| Disambiguation default-fill vs partial application | Met | `akkado/src/analyzer.cpp:52-114` (`is_placeholder_node`, `all_placeholders_have_defaults`) and `analyzer.cpp:763-764, 407-409` gate partial-application rewriting on whether all `_` positions have defaults |
| `_` as reserved identifier (not assignable) | Met | `akkado/src/parser.cpp:245-248` (const assign), `parser.cpp:268-275` (regular assign). Function-name rejection handled similarly (per existing test `test_parser.cpp:1205`) |
| Parser treats standalone `_` as `Identifier("_")` | Met | `akkado/src/parser.cpp:476-482`, verified by `test_parser.cpp:1178-1191` |

## Findings

### Unmet Goals
- **E107 named-argument rejection not implemented.** The PRD §2.4 and §8 require that `delay(%, time: _)` produces error E107. No such check appears in `codegen.cpp` or `codegen_functions.cpp`. Because `Argument` nodes carry an optional name in `Node::ArgumentData`, and codegen's placeholder detection inspects only the inner `Identifier` node, a named `_` silently degrades to default-filling. This is a missed error case.
- **Error-code collision on E107.** `codegen.cpp:917` already uses `E107` for "Unknown function: …", and `docs/prd-error-handling-recovery.md:280, 425, 449, 514` codify that assignment. The PRD reserves E107 for named-arg placeholder errors. Either the PRD should pick a different code (e.g., E108/E170) or `codegen.cpp:917` must be renumbered. This must be resolved before the named-arg check can land safely.

### Stubs
- None detected. Default-filling and partial-application code paths are fully fleshed out.

### Coverage Gaps
- **No codegen tests for underscore default-filling.** A grep of `akkado/tests/test_codegen.cpp` finds zero tests exercising `delay(%, 0.25, _, _, 0.8)`, `lp(%, 5000, _)`, `adsr(gate, _, _, 0.8)`, or user-function `_` usage (PRD §7.1 and §7.2). Only `test_parser.cpp:1178-1214` covers the parser side. Checklist items 6–9 in PRD §8 are unchecked and have no backing test.
- **No bytecode equivalence test.** PRD §7.1 asks for `lp(…, 5000, _)` vs `lp(…, 5000)` to produce identical bytecode. Not present anywhere in `akkado/tests/`.

### Missing Tests
- Builtin skip-middle test (`delay(%, 0.25, _, _, 0.8)`).
- Required-param error test (E106).
- Named-arg error test (E107) — blocked by unimplemented feature and error-code collision.
- All-placeholder test (`lp(%, _, _)`, PRD §6.1).
- User fn default-fill tests (numeric and expression defaults, PRD §6.4/§7.2).
- Redundant-trailing-underscore test (PRD §6.6).
- Rest-parameter rejection test (PRD §6.3) — rest handling at `codegen_functions.cpp:172-181` does not appear to detect a placeholder in the rest slot.

### Scope Drift
- PRD §4 Impact Table claims `parser.cpp` requires "No change," but `parser.cpp:245-275` contains new "Cannot assign to reserved identifier '_'" error paths that were required by PRD §2.5 and which are exercised by `test_parser.cpp:1193-1209`. This is additive work the PRD should have catalogued.
- PRD §4 Impact Table claims `analyzer.cpp` is unchanged. In fact the disambiguation work in `analyzer.cpp:52-114, 383-437, 736-810` is substantial and is essential for correct behavior. The PRD header note ("Implemented with smart disambiguation") acknowledges this retroactively but the File-Level Changes table (§5) still omits `analyzer.cpp`.

### Suggestions
1. Renumber either the PRD's named-arg error (use E108 or E170) or `codegen.cpp:917`'s "Unknown function" code; update `prd-error-handling-recovery.md` accordingly.
2. Add the E107-equivalent check: when iterating `Argument` children, inspect `ArgumentData.name.empty()`; if the wrapped inner is `Identifier("_")` and the name is non-empty, emit the error.
3. Fill out `test_codegen.cpp` with the cases listed in PRD §7 and §8.
4. Update PRD §4 and §5 tables to reflect the actual parser and analyzer changes that shipped.
5. Tick the PRD §8 checklist boxes for items genuinely completed (1, 2, 4) and leave 3/6–10 open until tests and E107 land.

## PRD Status
- Current: `DONE` (per line 1 header)
- Recommended: `Mostly Done — Tests and E107 Outstanding`
- Reason: The headline "smart disambiguation" feature is implemented and the parser/analyzer/codegen wiring is correct. However (a) the E107 named-argument error is neither implemented nor free of collision with the existing E107 "Unknown function" code, and (b) no codegen tests exist for the feature, leaving all §7 verification unautomated. Downgrading from DONE prevents the gap from being forgotten.

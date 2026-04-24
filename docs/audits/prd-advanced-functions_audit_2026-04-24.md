# Audit: Advanced Function System PRD

**PRD:** `docs/prd-advanced-functions.md`
**Audit base:** `bfd896ef7c13fc6f91bf9b4621bde22314d07799 (2026-02-09)` (creation commit; originally `docs/PRD-Advanced-Functions.md`, renamed by `4977c77` on 2026-04-24)
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit (no tests run, no fixes made)

## Summary
- Goals met: 6 of 6 code features (5 of 6 phases including Phase 6 docs)
- Critical findings: 3 (Unmet=0, Stubs=0, Coverage Gaps=0, Missing Tests=0, Doc Drift=2, Status Drift=1)
- Recommended PRD Status: MOSTLY COMPLETE
- One-line verdict: All six features ship with targeted tests, but the Phase 6 documentation work was skipped and the existing user-facing docs still contradict the new capabilities, so the DONE banner is premature.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| 1. String default parameters | Met | `akkado/include/akkado/ast.hpp:169`, `akkado/include/akkado/symbol_table.hpp:20-21`, `akkado/src/parser.cpp:820-876`, `akkado/src/codegen_functions.cpp:231,325,541`, `akkado/src/analyzer.cpp:322,462,544` |
| 2. Named arguments for user functions | Met | `akkado/include/akkado/analyzer.hpp:105` (generalized overload), `akkado/src/analyzer.cpp:1211` (user-fn call site), `akkado/src/analyzer.cpp:2066` (impl), existing builtin path at `analyzer.cpp:1304` |
| 3. Closures as return values | Met | `akkado/include/akkado/codegen.hpp:518` (`pending_function_ref_`), `akkado/src/codegen_functions.cpp:388-404`, `akkado/src/codegen.cpp:633-635` (Assignment handler pulls pending ref into FunctionValue), `FunctionRef.captured_literals` in `symbol_table.hpp` |
| 4. Variadic rest parameters | Met | `akkado/include/akkado/token.hpp:80` (`DotDotDot`), `akkado/src/lexer.cpp:215`, `akkado/src/parser.cpp:820`, `akkado/include/akkado/ast.hpp:170,233` (`is_rest`, `has_rest_param`), `akkado/src/codegen_functions.cpp:172,345` (synthetic Array binding), `akkado/src/analyzer.cpp:1237` (min-arg validation) |
| 5. Partial application | Met | `akkado/src/parser.cpp:476` (`Underscore` -> `Identifier("_")`), `akkado/src/analyzer.cpp:52,73` (placeholder helpers), `akkado/src/analyzer.cpp:383-432` (classification in `collect_definitions`), `akkado/src/analyzer.cpp:736` (pass-2 rewrite) |
| 6. Function composition via `compose()` | Met | `akkado/include/akkado/symbol_table.hpp:88` (`compose_chain`), `akkado/include/akkado/codegen.hpp:460` (`handle_compose_call`), `akkado/src/codegen_functions.cpp:633-665` (builder) + `akkado/src/codegen_functions.cpp:579-586` (chain execution), `akkado/src/codegen.cpp:848` (registered in `special_handlers`) |

## Findings

### Unmet Goals
- None.

### Stubs
- None. `grep -E 'TODO|FIXME|XXX|HACK|not implemented|unimplemented'` across `akkado/src/{codegen.cpp,codegen_functions.cpp,analyzer.cpp,parser.cpp,lexer.cpp}` and the associated headers returned zero hits on the PRD surface.

### Coverage Gaps
- None identified for the shipped feature set.

### Missing Tests
- None on the core features. Each goal has dedicated Catch2 tests:
  - Goal 1: `TEST_CASE("String default parameters", "[akkado][fn][string-defaults]")` with 3 SECTIONs -- `akkado/tests/test_akkado.cpp:1542-1587`
  - Goal 2: `TEST_CASE("Named arguments for user functions", "[akkado][fn][named-args]")` with 3 SECTIONs -- `akkado/tests/test_akkado.cpp:1589-1627`
  - Goal 3: `TEST_CASE("Closures as return values", "[akkado][fn][closure-return]")` with 3 SECTIONs -- `akkado/tests/test_akkado.cpp:1629-1666`
  - Goal 4: `TEST_CASE("Variadic rest parameters", "[akkado][fn][variadic]")` with 4 SECTIONs -- `akkado/tests/test_akkado.cpp:1668-1722`
  - Goal 5: `TEST_CASE("Partial application", "[akkado][fn][partial]")` + `TEST_CASE("Underscore placeholder default-filling", ...)` -- `akkado/tests/test_akkado.cpp:1725-1846`
  - Goal 6: `TEST_CASE("Function composition", "[akkado][fn][compose]")` (compose two / with partial / three functions) -- `akkado/tests/test_akkado.cpp:1848-1890`
  - Parser tests: `TEST_CASE("Parser string default parameters", "[parser][fn]")` and `TEST_CASE("Parser rest parameters", "[parser][fn]")` -- `akkado/tests/test_parser.cpp:1114-1179`
- Minor gap: PRD Section 6.1 edge case "String-defaulted param used in non-match context should error" has no explicit negative test. Not blocking.

### Scope Drift
- **Phase 6 (documentation) not executed.** PRD Section 4 and the Phase 6 checklist require updating `docs/agent-guide-userspace-functions.md` and `web/static/docs/reference/language/closures.md` plus running `bun run build:docs`. Neither file was touched by the feature commit `bfd896e`:
  - `docs/agent-guide-userspace-functions.md:40` still states `Default values must be **numeric literals** (no expressions, no strings)` -- directly contradicted by shipped string defaults.
  - `docs/agent-guide-userspace-functions.md:100` lists `Numeric-only defaults` as a live constraint.
  - Neither doc mentions variadic rest, partial application `_`, `compose()`, or closure-returning functions.
- Minor naming divergence: PRD Section 2.3 calls the helper `build_closure_ref()`; the implementation reuses the pre-existing `resolve_function_arg()` at `akkado/src/codegen_functions.cpp:388`. Functionally equivalent.
- Bonus cleanup in `bfd896e` (polyphony PRD update, web WASM export pruning) is incidental, not scope drift.

### Suggestions
- Complete Phase 6: update `docs/agent-guide-userspace-functions.md` (remove "Numeric-only defaults" claim, document string defaults / variadic rest / partial app / compose / closure returns) and `web/static/docs/reference/language/closures.md`; run `bun run build:docs` to regenerate `web/src/lib/docs/lookup-index.ts`.
- Add a negative test for PRD Section 6.1: using a string-default-only param as an audio signal should produce a compile error.
- Align PRD helper name (`build_closure_ref`) with the actual `resolve_function_arg` used in `codegen_functions.cpp`, or leave a one-line comment in code pointing back to the PRD section.

## PRD Status
- Current: `> **Status: DONE** -- All 6 features implemented (string defaults, named args, closures, variadic, partial application, compose).`
- Recommended: `Status: MOSTLY COMPLETE`
- Reason: All six runtime/compiler features are in source with matching Catch2 tests and no stubs or TODOs. However, the Phase 6 documentation items in the PRD's own checklist are unchecked and the user-facing docs actively contradict the new behavior (still claim "Numeric-only defaults"). The DONE banner should either be softened or Phase 6 should be executed.

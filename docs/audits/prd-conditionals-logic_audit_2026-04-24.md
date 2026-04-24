# Audit: Conditionals and Logic

**PRD:** `docs/prd-conditionals-logic.md`
**Audit base:** `4977c77 (2026-04-24)` — the commit that first added the PRD file (renamed from uppercase). PRD was created already marked `Status: DONE`, so feature work predates this base; verification is against HEAD.
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 4/4 functional Goals met; 20/21 Phase checklist items complete.
- Critical findings: 4 (Unmet=0, Stubs=0, Coverage Gaps=1, Missing Tests=3)
- Recommended PRD Status: MOSTLY COMPLETE
- One-line verdict: Implementation is end-to-end wired (opcodes, VM dispatch, builtins, lexer, parser desugaring, codegen tests) but Phase 5 docs/F1 help is undone and several value-level test-plan items are only realized as codegen-presence checks, so `Status: DONE` is premature.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| G1 C-style infix (`> < >= <= == != && \|\| !`) | Met | Lexer `akkado/src/lexer.cpp:240-286`; token enum `akkado/include/akkado/token.hpp:120-127,139`; parser precedences `akkado/src/parser.cpp:120-127`; infix dispatch `akkado/src/parser.cpp:498-506`; binary desugar `akkado/src/parser.cpp:913-947`; prefix `!` desugar `akkado/src/parser.cpp:590-604`. |
| G2 Function-call syntax (`gt lt gte lte eq neq band bor bnot select`) | Met | Builtins `akkado/include/akkado/builtins.hpp:540,546,550,554,558,562,566,572,576,580`. |
| G3 Signal-rate operation | Met | Per-sample loops `cedar/include/cedar/opcodes/logic.hpp:20,37,49,61,73,85,97,113`; VM dispatch `cedar/src/vm/vm.cpp:898-927`. |
| G4 Boolean/epsilon convention | Met | `LOGIC_EPSILON = 1e-6f` at `cedar/include/cedar/opcodes/logic.hpp:11`, used at lines 91 and 103. |

## Findings

### Unmet Goals
None.

### Stubs
None found in logic.hpp, lexer, or parser (no TODO/FIXME/XXX/HACK/push_warning/placeholder returns in relevant files).

### Coverage Gaps
- Epsilon behavior not exercised. `akkado/tests/test_codegen.cpp:881-894,977-990` only check that `CMP_EQ`/`CMP_NEQ` opcodes are emitted for `5 == 5` and `5 != 10`; nothing verifies near-equal floats are treated equal (e.g. `0.1 + 0.2 == 0.3`). A regression to a stricter `==` or changed epsilon would pass.

### Missing Tests
- Signal-rate square-wave verification named verbatim in the Test Plan (`check_signal("osc(\"sin\", 1) > 0", …)`) — absent.
- Runtime value assertion for `select` including the negative-falsy case (`select(-1, 100, 50) == 50`) — absent; only opcode presence tested (`test_codegen.cpp:924-942`).
- Runtime value assertions for comparisons/logic (`check_output("gt(10, 5)", 1.0f)`, `band(1, 0) == 0`, `bnot(1) == 0`, etc.) — absent. All current `[codegen][conditionals]` cases are codegen-presence checks only; inverted truth polarity would still pass.

### Missing documentation (Phase 5)
- `web/static/docs/reference/language/operators.md:5` keywords list does not include comparison/logic/conditional/select.
- No `web/static/docs/reference/builtins/conditionals.md` (or similar) exists.
- F1 lookup has no entries for `select`, `gt`, `lt`, `gte`, `lte`, `eq`, `neq`, `band`, `bor`, `bnot` in `web/src/lib/docs/lookup-index.ts` or `manifest.ts`.

### Scope Drift
None. Files changed since base (`akkado/tests/test_codegen.cpp`, `docs/MVP-INCOMPLETE-IMPLEMENTATIONS.md`, `docs/audits/MVP-INCOMPLETE-IMPLEMENTATIONS_audit_2026-04-24.md`) are unrelated to this PRD; all conditionals code predates the audit base.

### Suggestions
1. Resolve the `Status: DONE` vs 21 unchecked boxes contradiction by either checking them off or downgrading Status.
2. Convert codegen-presence tests into value-comparison tests (0.0/1.0 outputs, negative-falsy `select`, epsilon equality).
3. Add a signal-rate test for `osc("sin", 1) > 0` producing a square wave as the Test Plan demanded.
4. Add docs page/section and F1 lookup entries for the ten new builtins + five new operators, then re-run `bun run build:docs`.

## PRD Status
- Current: `DONE`
- Recommended: `MOSTLY COMPLETE`
- Reason: Core implementation shipped and tested for codegen, but Phase 5 documentation is entirely undone and the PRD's own Test Plan value assertions are not realized.

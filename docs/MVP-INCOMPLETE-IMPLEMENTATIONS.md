> **Status: COMPLETE** — All concrete MVP blockers resolved. Remaining items are informational.

# MVP Incomplete Implementations

This document tracks the incomplete implementations that were flagged as MVP blockers. Every concrete blocker has been resolved — either directly (Priority 1), or as a side effect of later refactors (TypedValue system enabling nested field access and UFCS, `ARRAY_INDEX` opcode added to Cedar VM). Remaining items (post statements, chord expansion stub) are either non-blocking or deliberately deferred.

Audit trail: [`audits/MVP-INCOMPLETE-IMPLEMENTATIONS_audit_2026-04-24.md`](audits/MVP-INCOMPLETE-IMPLEMENTATIONS_audit_2026-04-24.md).

## Priority 1: Pattern Transformations (Partially Complete)

Pattern transformation functions are implemented but have limitations:

### 1.1 Chained Transformations

**Status:** FIXED

Chaining for slow/fast/rev/transpose was already working. Chaining with velocity/bank/n as inner transforms was broken (`compile_pattern_for_transform()` only recognized slow/fast/rev/transpose). Now fixed — velocity/bank/n are recognized as recursive transforms.

### 1.2 Direct String Literal Syntax

**Status:** FIXED

`StringLit` nodes are now accepted by both `is_pattern_expr()` and `compile_pattern_for_transform()`. String literals are parsed as mini-notation via `parse_mini()`. `slow("c4 e4 g4", 2)` now works.

---

## Priority 2: Blocked Language Features

These features emit errors and are completely blocked.

### 2.1 Nested Field Access

**Status:** RESOLVED (via TypedValue refactor, commit `c733f66`)
**File:** `akkado/src/codegen.cpp:1860-1988` (`handle_field_access`)

```akkado
// Now works:
inner = {a: 11, b: 22}
outer = {x: inner, y: 99}
outer.x.a     // 11
outer.x.b     // 22
```

`handle_field_access` dispatches on the `TypedValue::type` of the child expression. When the child is a `Record`, each field lookup returns another `TypedValue` — so `a.b.c` recurses naturally. The E134 error site has been removed entirely.

**Tests:** `akkado/tests/test_codegen.cpp:2553-2609` (SECTION "nested record field access returns correct inner value", "nested record field access selects correct field (not first)", "triply nested record field access").

### 2.2 Method Calls

**Status:** RESOLVED via UFCS
**File:** `akkado/src/analyzer.cpp:1098` (`desugar_method_call`)

```akkado
// All of these now compile to the same bytecode as their functional form:
osc("saw", 440).lp(800).abs() |> out(%, %)          // == abs(lp(osc(...), 800))
pat("c4 e4 g4").slow(2)                              // == slow(pat("c4 e4 g4"), 2)
pat("c4 e4").fast(2).slow(4)                         // == slow(fast(pat("c4 e4"), 2), 4)
osc("saw", 440) |> %.lp(800) |> out(%, %)            // dot-call on hole
osc("saw", 440) as q |> q.lp(800) |> out(%, %)       // dot-call on as-binding
```

The analyzer's `desugar_method_call()` rewrites `a.f(b)` → `f(a, b)` during the lowering pass. The codegen `E113` branch at `codegen.cpp:1511` now only functions as a defensive guard for un-rewritten `MethodCall` nodes — which shouldn't occur in practice.

**Tests:** `akkado/tests/test_codegen.cpp:1760-1898` (chained dot-calls, user fn dot-calls, dot-call on hole, dot-call on as-binding, pattern method via dot-call, chained pattern methods).

Known edge: dot-call on a variable-bound pattern (`p = pat(...); p.slow(2)`) currently errors with E130 inside the transform pipeline — the method call is rewritten, but the pattern-arg compiler doesn't follow the variable binding. Not an MVP blocker; track separately if needed.

### 2.3 Post Statements

**Status:** BLOCKED (E115) — non-blocking for MVP
**File:** `akkado/src/codegen.cpp:1518-1520`
**Syntax:** `post(() -> ...)`

**Error:** "Post statements not supported in MVP"

**Investigation needed:** What is the intended semantics of post statements? Not used anywhere in shipping patches, so this remains a no-op error until the semantics are specified.

### 2.4 Field Access on Arbitrary Expressions

**Status:** LARGELY RESOLVED (via TypedValue refactor); edge cases still error
**File:** `akkado/src/codegen.cpp:1860-1988` (`handle_field_access`)

`handle_field_access` now type-dispatches the child expression and supports field access on:
- `Record` (including nested records — see §2.1)
- `Pattern` (fields `freq`, `vel`, `trig`, `gate`, `type`)

Remaining hard-errors (which are correct, not blockers):
- `E135` on `Signal`, `Number`, `Array`, `Function`, `String`, `Void` — these don't have fields.
- `E061` (analyzer) on direct scalar field access like `x.field` where `x = 42`.

Both are intentional — you can't take `.field` of a scalar.

---

## Priority 3: Stub Implementations

### 3.1 Chord Expansion (ChordLit)

**Status:** OBSOLETE — `C4'` standalone syntax deprecated in favor of chords inside `pat()`/`chord()`
**File:** `akkado/src/codegen.cpp:167-180`

```akkado
// C4' standalone syntax only emits root note — this is expected
// Use chords inside patterns instead:
pat("C4'") |> sine(%.freq) |> sum(%) |> out(%, %)  // Works correctly (multi-voice)
chord("Am") |> ...                                    // Also works
```

**Resolution:** The PRD ([prd-pattern-array-note-extensions.md](prd-pattern-array-note-extensions.md)) explicitly deprecates standalone `C4'` syntax in favor of `chord("Am")` inside mini-notation. Chords in patterns already expand correctly to multi-voice via multi-buffer support. No fix needed for the standalone stub.

### 3.2 Array Indexing

**Status:** RESOLVED
**File:** `akkado/src/codegen.cpp:294-420` (index handling in `visit`)

```akkado
arr = [10, 20, 30]
arr[0]  // 10
arr[1]  // 20 (constant-folded to PUSH_CONST)
arr[2]  // 30
arr[freq]  // dynamic index: emits ARRAY_INDEX with ARRAY_PACK
```

Cedar gained an `ARRAY_INDEX` opcode. Codegen folds constant indices into a direct `PUSH_CONST` of the element (line 300-302); dynamic indices pack the array into a single buffer (`ARRAY_PACK` / `ARRAY_PUSH`) and emit `ARRAY_INDEX` with wrap mode (line 407-417).

**Tests:** `akkado/tests/test_codegen.cpp:3503-3557` (SECTION "constant index returns the indexed element, not first", "last element accessible via constant index", "index 0 returns first element", "dynamic index emits ARRAY_INDEX opcode").

---

## Implementation Order

1. ~~**Pattern transformation chaining**~~ - **FIXED** (commit `9a61089`)
2. ~~**Direct string literal syntax**~~ - **FIXED** (commit `9a61089`)
3. ~~**Nested field access**~~ - **RESOLVED** via TypedValue refactor (commit `c733f66`)
4. ~~**Field access on expressions**~~ - **RESOLVED** for Pattern/Record; E135 correctly fires for non-record types
5. ~~**Method calls (UFCS)**~~ - **RESOLVED** via `desugar_method_call` in analyzer
6. **Post statements** - still blocked, needs semantics clarification; not used anywhere
7. ~~**Array indexing**~~ - **RESOLVED** via `ARRAY_INDEX` opcode in Cedar VM
8. ~~**Chord expansion**~~ - **OBSOLETE** — `C4'` deprecated, chords in patterns work

---

## Test Commands

```bash
# Build akkado
cmake --build build --target akkado

# Run pattern tests
./build/akkado/tests/akkado_tests "[codegen][patterns]"

# Run all tests
./build/akkado/tests/akkado_tests
```

---

## Files That Shipped The Fixes

| Issue | Primary File | Commit |
|-------|--------------|--------|
| Pattern chaining (velocity/bank/n) | `akkado/src/codegen_patterns.cpp` | `9a61089` |
| String literal patterns | `akkado/src/codegen_patterns.cpp` | `9a61089` |
| Nested field access | `akkado/src/codegen.cpp` (`handle_field_access`) | `c733f66` (TypedValue refactor) |
| Method calls (UFCS) | `akkado/src/analyzer.cpp` (`desugar_method_call`) | — |
| Field access on expr | `akkado/src/codegen.cpp` (type-dispatched) | `c733f66` |
| Array indexing | `cedar/src/vm/` (`ARRAY_INDEX`), `akkado/src/codegen.cpp` | — |

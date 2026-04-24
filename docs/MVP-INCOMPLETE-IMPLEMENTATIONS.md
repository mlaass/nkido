> **Status: TRACKING** — Active blockers for MVP completeness.

# MVP Incomplete Implementations

This document tracks all incomplete implementations in the akkado codebase that need to be fixed.

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

**Status:** BLOCKED (E134)
**File:** `akkado/src/codegen.cpp:915-923`

```akkado
// This FAILS:
record.field1.field2
```

**Error:** "Nested field access not fully supported in MVP"

**Solution:** Track intermediate record types during field resolution. When resolving `a.b.c`:
1. Resolve `a.b` to get its type
2. Use that type to resolve `.c`

### 2.2 Method Calls

**Status:** DEFERRED BY DESIGN (E113)
**File:** `akkado/src/codegen.cpp:648-650`

```akkado
// This FAILS:
pattern.slow(2)
```

**Error:** "Method calls not supported in MVP"

**Note:** This is intentionally deferred, not just blocked. Method chaining should be designed holistically as part of an object system revamp to work consistently across patterns, arrays, chords, and audio signals. See [prd-pattern-array-note-extensions.md Section 8](prd-pattern-array-note-extensions.md#8-deferred-to-object-system-revamp) for rationale. The functional style (`slow(pat(...), 2)`) is the current endorsed approach.

**Solution Options (for future object system revamp):**
- **UFCS (Uniform Function Call Syntax):** Rewrite `x.f(args)` to `f(x, args)` during parsing/analysis
- **Full method dispatch:** Implement proper method resolution

**Recommendation:** UFCS is simpler and sufficient for Akkado's use case.

### 2.3 Post Statements

**Status:** BLOCKED (E115)
**File:** `akkado/src/codegen.cpp:655-657`

**Error:** "Post statements not supported in MVP"

**Investigation needed:** What is the intended semantics of post statements?

### 2.4 Field Access on Arbitrary Expressions

**Status:** BLOCKED (E135)
**File:** `akkado/src/codegen.cpp:924-928`

```akkado
// This FAILS:
(some_expr).field
```

**Error:** "Field access on expression type not supported"

**Solution:**
1. Evaluate the expression
2. Determine its type (must be a record type)
3. Resolve the field access on that type

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

**Status:** LOW PRIORITY — stub returns first element
**File:** `akkado/src/codegen.cpp:226-240`

```akkado
arr = [1, 2, 3]
arr[1]  // Should return 2, currently returns 1
```

**Problem:** No runtime array indexing opcode in Cedar VM.

**Blocker:** Requires Cedar VM changes to add an `ARRAY_INDEX` opcode.

**Note:** The multi-buffer approach for polyphony covers the primary use case for arrays. Pattern voices are accessed via field names (`%.freq`, `%.vel`), not array indices. This reduces the urgency of runtime array indexing.

---

## Implementation Order

1. ~~**Pattern transformation chaining**~~ - **FIXED**
2. ~~**Direct string literal syntax**~~ - **FIXED**
3. **Nested field access** - akkado-only, type tracking change
4. **Field access on expressions** - akkado-only, type tracking change
5. **Method calls (UFCS)** - deferred by design to object system revamp
6. **Post statements** - needs semantics clarification first
7. **Array indexing** - low priority, multi-buffer approach covers primary use case
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

## Files to Modify

| Issue | Primary File | Secondary Files |
|-------|--------------|-----------------|
| Pattern chaining | `akkado/src/codegen_patterns.cpp` | - |
| String literal patterns | `akkado/src/codegen_patterns.cpp` | - |
| Nested field access | `akkado/src/codegen.cpp` | `akkado/include/akkado/codegen.hpp` |
| Method calls (UFCS) | `akkado/src/parser.cpp` or `akkado/src/analyzer.cpp` | - |
| Field access on expr | `akkado/src/codegen.cpp` | - |
| Array indexing | `cedar/src/vm/` | `akkado/src/codegen.cpp` |
| Chord expansion | `akkado/src/codegen.cpp` | Cedar multi-buffer support |

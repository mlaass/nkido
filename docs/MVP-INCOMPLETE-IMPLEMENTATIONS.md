> **Status: TRACKING** — Active blockers for MVP completeness.

# MVP Incomplete Implementations

This document tracks all incomplete implementations in the akkado codebase that need to be fixed.

## Priority 1: Pattern Transformations (Partially Complete)

Pattern transformation functions are implemented but have limitations:

### 1.1 Chained Transformations Not Working

**Status:** BROKEN
**File:** `akkado/src/codegen_patterns.cpp:1165-1227`

```akkado
// This works:
slow(pat("c4 e4 g4"), 2)

// This FAILS with E130:
transpose(slow(pat("c4 e4"), 2), 12)
```

**Problem:** `compile_pattern_for_transform()` only handles:
- Direct `MiniLiteral` nodes
- `Call` nodes with `MiniLiteral` as first argument

It doesn't handle `Call` nodes where the first argument is another transformation call.

**Solution:** Implement recursive pattern compilation:
1. When the argument is a transformation call (slow/fast/rev/transpose/velocity), recursively compile it
2. Extract the resulting `sequence_events` from the inner compilation
3. Apply the outer transformation to those events
4. Emit only once at the outermost level

### 1.2 Direct String Literal Syntax Not Working

**Status:** BROKEN
**File:** `akkado/src/codegen_patterns.cpp:1165-1227`

```akkado
// This works:
slow(pat("c4 e4 g4"), 2)

// This FAILS with E133:
slow("c4 e4 g4", 2)
```

**Problem:** `compile_pattern_for_transform()` doesn't handle `StringLit` nodes.

**Solution:** Add handling for `StringLit` nodes:
1. Check if `pat_node.type == NodeType::StringLit`
2. Parse the string content as mini-notation using `parse_mini()`
3. Compile the resulting pattern AST

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

**Status:** BLOCKED (E113)
**File:** `akkado/src/codegen.cpp:648-650`

```akkado
// This FAILS:
pattern.slow(2)
```

**Error:** "Method calls not supported in MVP"

**Solution Options:**
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

**Status:** STUB - only emits root note
**File:** `akkado/src/codegen.cpp:167-180`

```akkado
// C4' should emit [261.6, 329.6, 392.0] Hz (C major chord)
// Currently only emits 261.6 Hz (root note)
```

**Problem:** Full chord expansion requires array support in Cedar VM for polyphonic playback.

**Blocker:** Needs Cedar-level array/multi-buffer support or parallel oscillator emission.

**Current workaround:** Chords in mini-notation patterns DO work (they use multi-buffer support). Only standalone `ChordLit` nodes are affected.

### 3.2 Array Indexing

**Status:** STUB - returns first element
**File:** `akkado/src/codegen.cpp:226-240`

```akkado
arr = [1, 2, 3]
arr[1]  // Should return 2, currently returns 1
```

**Problem:** No runtime array indexing opcode in Cedar VM.

**Blocker:** Requires Cedar VM changes to add an `ARRAY_INDEX` opcode.

---

## Implementation Order

1. **Pattern transformation chaining** - akkado-only, unblocks common use case
2. **Direct string literal syntax** - akkado-only, small change
3. **Nested field access** - akkado-only, type tracking change
4. **Field access on expressions** - akkado-only, type tracking change
5. **Method calls (UFCS)** - parser/analyzer change, enables fluent API
6. **Post statements** - needs semantics clarification first
7. **Array indexing** - requires Cedar VM changes
8. **Chord expansion** - requires Cedar array support

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

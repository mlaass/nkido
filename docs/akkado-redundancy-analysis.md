# Akkado Compiler: Redundancy & Bloat Analysis

## Context

The akkado compiler source totals ~18,300 lines across 23 `.cpp` files. The question: why are `analyzer.cpp` (2,166 lines) and `parser.cpp` (1,574 lines) both so large, and is there redundancy between them?

**Short answer:** Parser and analyzer have proper separation of concerns -- parser does syntax, analyzer does semantics. They are NOT redundant with each other. However, both files contain significant *internal* duplication, and the real redundancy problem is between **analyzer + codegen** -- the codegen layer re-validates much of what the analyzer already checked.

---

## 1. Parser vs Analyzer: NOT Redundant

| Responsibility | Parser (1,574 lines) | Analyzer (2,166 lines) |
|---|---|---|
| Token -> AST | Yes | No |
| Operator precedence | Yes | No |
| Syntax error recovery | Yes | No |
| Symbol registration | No | Yes (pass 1) |
| Pipe rewriting / desugaring | No | Yes (pass 2) |
| Method call desugaring | No | Yes (pass 2) |
| Partial application | No | Yes (pass 2) |
| Symbol resolution | No | Yes (pass 3) |
| Arg count validation | No | Yes (pass 3) |
| Named arg reordering | No | Yes (pass 3) |
| Const purity checking | No | Yes (pass 3) |
| Closure capture validation | No | Yes (pass 3) |

The only micro-overlap: duplicate record field checking (parser line 1525) -- this is appropriate at parse time.

**Why the analyzer is large:** It's not just a validator. It performs major AST transformations (pipe rewriting, method desugaring, closure creation) alongside semantic analysis. "Analyzer" undersells what it does -- it's closer to a "semantic transformer."

---

## 2. Internal Duplication in `analyzer.cpp`

### A. Named argument reordering: 260 lines of near-identical code

Two overloads of `reorder_named_arguments()`:
- **Lines 1906-2033** (builtin version): takes `const BuiltinInfo&`, uses `builtin.find_param(name)`
- **Lines 2035-2164** (user-fn version): takes `const vector<string>&`, uses linear search

The two functions are **~95% identical**. The only difference is how parameter index lookup works (one method call vs one loop). This could be a single templated or callback-parameterized function.

**Savings: ~120 lines**

### B. Parameter extraction pattern: repeated 4+ times

The same logic to extract `FunctionParamInfo` from `ClosureParamData`/`IdentifierData` AST nodes appears at lines ~295, ~436, ~510, and ~1373. Each instance is 20-30 lines of identical struct-field copying.

**Savings: ~60-80 lines**

### C. Record field collection: duplicated 2x

Identical logic to process spread sources + explicit field overrides in:
- `collect_definitions()` (lines ~223-286)
- `resolve_and_validate()` (lines ~1535-1592)

**Savings: ~60 lines**

### D. Placeholder/hole detection: inlined 3x

`is_placeholder_node()` exists as a static helper but equivalent checks are inlined at lines ~712 and ~365.

**Savings: ~30 lines**

### E. Oversized functions

| Function | Lines | Problem |
|---|---|---|
| `resolve_and_validate()` | 573 | Handles 8+ node types in one function |
| `collect_definitions()` | 367 | Mixes variable/array/record/closure/pipe logic |
| `clone_subtree()` | 175 | Handles 6 different node rewrites |

**Total estimated bloat in analyzer: ~300-350 lines (14-16% of file)**

---

## 3. Internal Duplication in `parser.cpp`

### A. Parameter node building: duplicated in closure vs fn_def

- `parse_closure()` lines 770-804: manual first/prev sibling linking
- `parse_fn_def()` lines 1434-1452: same logic using `arena_.add_child()`

Identical `ClosureParamData` vs `IdentifierData` branching, identical default-node attachment. Should be one `build_param_nodes()` helper.

**Savings: ~20 lines**

### B. Destructuring field list parsing: duplicated 2x

- `parse_precedence()` lines ~384-395
- `parse_match_expr()` lines ~1255-1266

Same while-loop consuming `Identifier` tokens separated by commas inside braces.

**Savings: ~12 lines**

### C. Argument wrapping boilerplate: repeated 6+ times

```cpp
NodeIndex arg = arena_.alloc(NodeType::Argument, loc);
arena_[arg].data = Node::ArgumentData{std::nullopt};
arena_.add_child(arg, expr);
```

Appears in unary_not, binary ops, call sites. A 1-line helper would cut ~15 lines.

### D. Binary op name switch: could be a lookup table

Lines 913-933: 13-case switch mapping `TokenType` to function name string. Could be a `constexpr` array.

**Total estimated bloat in parser: ~60-80 lines (4-5% of file)**

The parser is relatively clean. Its size is justified by the grammar complexity.

---

## 4. The Real Problem: Codegen Re-validates What Analyzer Already Checked

This is the largest source of redundancy in the codebase. The codegen layer (~8,700 lines across 7 files) systematically re-checks things the analyzer's pass 3 should have caught:

### A. Argument count re-validation (pervasive)

The analyzer validates builtin arg counts in `validate_arguments()`. Despite this, codegen files contain **dozens** of redundant arg-count checks:

- `codegen_arrays.cpp`: ~20 functions re-check arg counts (map, fold, zipWith, sum, take, drop, etc.)
- `codegen_params.cpp`: param/button/toggle/select all re-check
- `codegen_functions.cpp`: tap_delay, poly, compose re-check
- `codegen_patterns.cpp`: slow, fast, rev, transpose all re-check

### B. Type constraint enforcement in codegen

Codegen validates argument types (must be string literal, must be number literal, must be pattern) that are statically determinable from the AST:

- **String literal checks**: `codegen_params.cpp` checks param/toggle/button names are StringLit
- **Number literal checks**: `codegen_arrays.cpp` checks take/drop/repeat/spread counts
- **Pattern checks**: `codegen_patterns.cpp` checks slow/fast/rev/transpose first args are patterns

All of these could be enforced in the analyzer's pass 3, making codegen simpler and errors earlier.

### C. Closure parameter count checks in codegen

- `codegen_arrays.cpp:80-86`: lambda needs >= 1 param
- `codegen_arrays.cpp:217-219`: binary function needs >= 2 params
- `codegen_functions.cpp:1323`: tap_delay processor needs exactly 1 param
- `codegen_functions.cpp:1527`: poly instrument needs exactly 3 params

These are all semantic constraints that belong in the analyzer.

### D. Identifier resolution in codegen

`codegen.cpp:421-428` checks for undefined identifiers -- this should have been caught in analyzer pass 3.

**Estimated redundant validation in codegen: 500-800 lines across all codegen files**

---

## 5. Summary

| Area | Bloat | Severity |
|---|---|---|
| Analyzer internal duplication | ~300-350 lines | Medium |
| Parser internal duplication | ~60-80 lines | Low |
| Codegen re-validating analyzer work | ~500-800 lines | **High** |
| **Total** | **~900-1,200 lines** | |

### Root cause

The analyzer (pass 3) doesn't validate *enough*. It checks arg counts and named args but doesn't enforce type constraints (string-literal-required, number-literal-required, pattern-required, closure-param-count). Because codegen can't trust these were checked, it defensively re-validates everything. This leads to:

1. Duplicate error messages and codes across analyzer + codegen
2. Errors reported later than necessary (at codegen instead of analysis)
3. Codegen bloated with validation logic instead of focused on instruction emission

### What this means for the parser/analyzer size question

- **Parser at 1,574 lines**: Justified. Grammar is complex (mini-notation, chords, match expressions, closures, pipes). Internal duplication is minor (~5%).
- **Analyzer at 2,166 lines**: Somewhat bloated internally (~15%), but its size is justified by doing 3 passes of real work (symbol collection, AST transformation, validation). The issue is it doesn't validate *enough*, pushing work to codegen.
- **The real bloat**: Lives in the codegen layer, where ~500-800 lines are redundant defensive validation.

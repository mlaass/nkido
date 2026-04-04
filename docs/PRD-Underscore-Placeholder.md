> **Status: NOT STARTED** — Design complete, no implementation yet.

# Underscore Placeholder PRD — Positional Default Skipping

## Executive Summary

This PRD specifies using `_` (underscore) as a positional placeholder in function call arguments to skip optional parameters and use their default values. This allows users to set a later optional parameter without manually specifying all preceding ones.

### Why?

Many DSP builtins have 3-5+ parameters where only the first few are required. Today, to set the 4th parameter you must explicitly pass the defaults for parameters 2 and 3:

```akkado
// Current: must know and repeat default values to set 'wet'
delay(%, 0.25, 0.0, 0.0, 0.8)

// Proposed: skip middle params, compiler fills defaults
delay(%, 0.25, _, _, 0.8)
```

This is especially valuable for DSP functions where default values are carefully tuned (filter Q, delay feedback, envelope times) and users shouldn't need to look them up just to reach a later parameter.

**Key design decisions:**
- `_` works for both **builtins** and **user-defined functions** with defaults
- `_` on a **required parameter** (no default) is a **compile error**
- `_` is only valid inside **function call argument lists** — error in other contexts
- Trailing arguments can still be omitted as today — `_` is only needed to skip **middle** parameters
- Multiple consecutive `_` are allowed (each skips one position)

---

## 1. Current State

### 1.1 Token

`TokenType::Underscore` already exists (`token.hpp:78`). The lexer recognizes standalone `_` as a distinct token (`lexer.cpp:176-179`). The parser currently converts it to `Identifier("_")` in expression contexts and uses it as a wildcard in `match` arms.

### 1.2 Optional Parameters

Builtins define optional parameters via `BuiltinInfo.optional_count` and `BuiltinInfo.defaults[]` (`builtins.hpp:72-121`). When arguments are omitted at the end of a call, the compiler emits `PUSH_CONST` instructions for each default (`codegen.cpp:1045-1069`).

User functions define defaults via `FunctionParamInfo.default_value`, `.default_string`, or `.default_node` (`symbol_table.hpp:16-23`). Missing trailing arguments are filled from these defaults (`codegen_functions.cpp:165-279`).

### 1.3 Limitation

Today, defaults can only fill **trailing** missing arguments. There is no way to skip a middle parameter:

```akkado
// delay(in, time, fb, dry, wet) — dry defaults to 0.0, wet to 1.0
delay(%, 0.25, 0.8)          // OK: skips dry and wet (trailing)
delay(%, 0.25, _, _, 0.8)    // ERROR today: _ is not handled as skip
```

---

## 2. Target Syntax

### 2.1 Basic Usage

```akkado
// Skip optional middle parameters
delay(%, 0.25, _, _, 0.8)       // time=0.25, fb=default, dry=default, wet=0.8
lp(%, 5000, _)                  // cut=5000, q=default (0.707)
adsr(gate, _, _, 0.8)           // attack=default, decay=default, sustain=0.8
```

### 2.2 User-Defined Functions

```akkado
fn synth(freq, wave = "saw", cut = 2000, res = 0.5) =
    osc(wave, freq) |> lp(%, cut, res)

// Skip wave and cut, set res
synth(440, _, _, 0.9)

// Skip just wave
synth(440, _, 8000)
```

### 2.3 Trailing Omission Still Works

```akkado
// These remain equivalent — no trailing _ needed
delay(%, 0.25, 0.8)
delay(%, 0.25, 0.8, _, _)   // Valid but redundant
```

### 2.4 Error Cases

```akkado
// _ on required parameter — compile error
delay(_, 0.25, 0.8)
// Error: Cannot skip required parameter 'in' — no default value

// _ outside function call — compile error
x = _
// Error: '_' placeholder only valid in function arguments

// _ in named argument — compile error (positional only)
delay(%, time: _)
// Error: '_' placeholder not valid in named arguments
```

---

## 3. Architecture

### 3.1 AST Representation

No new AST node type needed. The parser already creates `Identifier("_")` for underscore in expression position. The codegen will detect this sentinel name when processing function arguments.

### 3.2 Compilation Flow

For a call like `delay(%, 0.25, _, _, 0.8)`:

```
1. Parser produces 5 argument nodes: [Hole, Number(0.25), Ident("_"), Ident("_"), Number(0.8)]
2. Codegen iterates arguments positionally against parameter list
3. For each argument:
   - If argument is Ident("_"):
     a. Look up parameter at this position
     b. If parameter has a default → emit PUSH_CONST with default value
     c. If parameter is required (no default) → emit compile error
   - Otherwise: visit argument normally
```

### 3.3 Detection

In codegen, when processing each argument node, check:

```cpp
bool is_placeholder = (arena_[arg_node].type == NodeType::Identifier &&
                        arena_[arg_node].as_identifier() == "_");
```

This check happens **before** normal identifier resolution, so `_` never reaches the symbol table lookup.

---

## 4. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `token.hpp` | **No change** | `TokenType::Underscore` already exists |
| `lexer.cpp` | **No change** | Already lexes `_` correctly |
| `parser.cpp` | **No change** | Already parses `_` as `Identifier("_")` |
| `codegen.cpp` | **Modified** | Builtin call: detect `_` args, fill defaults |
| `codegen_functions.cpp` | **Modified** | User fn call: detect `_` args, fill defaults |
| `match` wildcards | **No change** | Match arm parsing is a distinct code path |
| Mini-notation `~` | **No change** | Rest in patterns is a separate system |

---

## 5. File-Level Changes

| File | Change |
|------|--------|
| `akkado/src/codegen.cpp` | In builtin call compilation (~line 820-1089): when iterating argument nodes, detect `Identifier("_")` and substitute the builtin's default value. Error if parameter has no default. |
| `akkado/src/codegen_functions.cpp` | In user function call compilation (~line 165-279): same detection — check for `_` placeholder, use `FunctionParamInfo` default. Error if no default exists. |
| `akkado/src/codegen.cpp` | Add validation: if `_` appears outside a function call argument list (e.g. in assignment, array literal, pipe), emit error. |
| `akkado/tests/test_codegen.cpp` | Tests for builtin calls with `_`, user fn calls with `_`, error cases |
| `akkado/tests/test_parser.cpp` | Verify `_` in argument position parses correctly (may already pass) |

---

## 6. Edge Cases

### 6.1 All Arguments Are Placeholders

```akkado
lp(%, _, _)   // Both cut and q use defaults — equivalent to lp(%)
```
Valid. Each `_` fills its position's default.

### 6.2 Underscore as Variable Name

```akkado
_foo = 42        // Valid identifier starting with underscore
f(_foo)          // Passes variable _foo, NOT a placeholder
f(_)             // Placeholder — standalone _ only
```
No conflict. Only standalone `_` (lexed as `TokenType::Underscore`, parsed as `Identifier("_")`) triggers placeholder behavior. Identifiers starting with underscore like `_foo` are normal variables.

### 6.3 Variadic / Rest Parameters

```akkado
fn f(x, ...rest) = ...
f(1, _, 3)    // _ here is in rest position — no default exists
```
Compile error: rest parameters have no defaults. `_` is not valid in rest parameter position.

### 6.4 User Function with Expression Default

```akkado
fn f(x, y = x * 2) = x + y
f(10, _)    // Should use default: y = 10 * 2 = 20
```
Valid. Expression defaults are evaluated by `ConstEvaluator` — same path as trailing omission today.

### 6.5 Named Arguments Mixed with Positional

```akkado
delay(%, 0.25, _, wet: 0.8)   // Positional _ then named arg
```
If named arguments are supported: the `_` applies to positional parameter 2 (`fb`). Named args bypass positional ordering. `_` in a named arg position (`wet: _`) is an error since named args already skip by omission.

### 6.6 Redundant Trailing Underscores

```akkado
delay(%, 0.25, 0.8, _, _)   // Explicit _ for trailing params
```
Valid but redundant. Same result as `delay(%, 0.25, 0.8)`. No warning emitted.

---

## 7. Testing Strategy

### 7.1 Builtin Calls

```cpp
// _ skips middle optional param
auto r = compile("delay(osc(\"sin\", 440), 0.25, _, _, 0.8)");
CHECK(r.success);

// _ on required param → error
auto r2 = compile("delay(_, 0.25, 0.8)");
CHECK_FALSE(r2.success);
// Check for appropriate error code

// Same bytecode: explicit defaults vs _
auto r3 = compile("lp(osc(\"sin\", 440), 5000, _)");
auto r4 = compile("lp(osc(\"sin\", 440), 5000)");
CHECK(r3.bytecode.size() == r4.bytecode.size());
```

### 7.2 User-Defined Functions

```cpp
// _ with numeric default
auto r = compile("fn f(x, y = 10, z = 20) = x + y + z\nf(1, _, 30)");
CHECK(r.success);

// _ with expression default
auto r2 = compile("fn g(x, y = 100) = x * y\ng(5, _)");
CHECK(r2.success);

// _ on required user fn param → error
auto r3 = compile("fn h(x, y) = x + y\nh(1, _)");
CHECK_FALSE(r3.success);
```

### 7.3 Error Cases

```cpp
// _ outside function call
auto r = compile("x = _");
CHECK_FALSE(r.success);

// _ in array
auto r2 = compile("[1, _, 3]");
CHECK_FALSE(r2.success);
```

### 7.4 Build & Run

```bash
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[codegen]"
./build/akkado/tests/akkado_tests "[parser]"
```

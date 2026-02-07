# PRD: Akkado Compiler Type System

## Problem

The Akkado compiler lacks a type system. `visit()` returns `uint16_t` (a buffer index) with no type information. Structural metadata is tracked in six ad-hoc maps on the `CodeGenerator`:

| Map | Key | Value | Purpose |
|-----|-----|-------|---------|
| `node_buffers_` | NodeIndex | buffer | Visit cache |
| `multi_buffers_` | NodeIndex | vector\<buffer\> | Arrays, chords |
| `stereo_outputs_` | NodeIndex | {left, right} | Stereo pairs |
| `record_fields_` | NodeIndex | {name → buffer} | Record/pattern fields |
| `polyphonic_fields_` | NodeIndex | {freq[], vel[], trig[], gate[], type[]} | Per-voice pattern data |
| `array_lengths_` | buffer | uint8 | Array element count |

This causes three concrete problems:

1. **No type checking on builtin arguments.** `BuiltinInfo` specifies `input_count` and `optional_count` but not parameter types. A user can pass a Pattern where a Signal is expected — the compiler emits garbage silently.

2. **Field access relies on string matching.** `handle_field_access()` (codegen.cpp:1206-1389) checks five cases (record literal, pattern literal, identifier, call, nested) by probing the ad-hoc maps. Adding a new compound type means adding another case and another map.

3. **New features can't inspect argument types.** A `transport()` builtin needs to know its first arg is a Pattern to compile the inner sequence and wire up clock override. Today there is no mechanism for this — the codegen would need yet another ad-hoc map.

## Proposed Types

Built-in, not user-extendable. Seven types:

| Type | Representation | Examples |
|------|---------------|----------|
| **Signal** | Single audio-rate buffer | `osc("sin", 440)`, `% * 0.5` |
| **Number** | Compile-time constant (float) | `440`, `0.5`, `2 * pi` |
| **Pattern** | Record of {freq, vel, trig, gate, type} + SequenceState ref + cycle_length | `pat("c4 e4 g4")`, `seq("x _ x _")` |
| **Record** | Named fields → TypedValue | `{freq: 440, vel: 0.8}` |
| **Array** | Ordered typed values | `[1, 2, 3]`, chord expansion `C4'` |
| **String** | Compile-time string literal | `"sin"`, `"kick.wav"` |
| **Void** | No value (side effects) | `out(sig, sig)` |

Pattern is a specialization of Record with known field names and associated SequenceState. The compiler may treat Pattern as a Record subtype for field access purposes.

## Core Change: TypedValue

```cpp
enum class ValueType : uint8_t {
    Signal,
    Number,
    Pattern,
    Record,
    Array,
    String,
    Void
};

struct TypedValue {
    ValueType type;
    uint16_t buffer;  // Primary buffer (Signal, Number, Void=UNUSED)

    // Compound type payload — one of:
    //   Pattern:  PolyphonicFields* + state_id + cycle_length
    //   Record:   field name → TypedValue map (small_map or pointer)
    //   Array:    vector<TypedValue>
    //   String:   interned string ID
    // Stored via tagged union or discriminated variant.
};
```

`visit()` returns `TypedValue` instead of `uint16_t`.

### TypedValue for each type

**Signal:**
```cpp
TypedValue { .type = Signal, .buffer = buf_idx }
```

**Number:**
```cpp
TypedValue { .type = Number, .buffer = const_buf_idx }
// const_buf_idx points to a buffer filled with the constant value
// Alternatively, store the float directly and allocate buffer lazily
```

**Pattern (monophonic):**
```cpp
TypedValue { .type = Pattern, .buffer = freq_buf,
    .pattern = { .fields = {freq: freq_buf, vel: vel_buf, trig: trig_buf, gate: gate_buf, type: type_buf},
                 .state_id = state_id,
                 .cycle_length = 4.0f,
                 .num_voices = 1 } }
```

**Pattern (polyphonic):**
Same as above but each field has N buffers (one per voice), and `num_voices > 1`.

**Record:**
```cpp
TypedValue { .type = Record, .buffer = first_field_buf,
    .record = { {"freq", TypedValue{Signal, buf1}}, {"vel", TypedValue{Signal, buf2}} } }
```

**Array:**
```cpp
TypedValue { .type = Array, .buffer = elements[0].buffer,
    .array = { TypedValue{Signal, buf1}, TypedValue{Signal, buf2}, TypedValue{Signal, buf3} } }
```

**String:**
```cpp
TypedValue { .type = String, .buffer = BUFFER_UNUSED,
    .string_id = interned_id }
```

**Void:**
```cpp
TypedValue { .type = Void, .buffer = BUFFER_UNUSED }
```

## Impact on Pipes and Holes

Today `%` (hole) is substituted at AST level by `substitute_holes()` in the analyzer (analyzer.cpp:381-646). The analyzer clones the replacement node. The codegen never sees a hole — it sees the substituted expression.

With TypedValue, **no change to hole substitution logic is needed.** The AST rewrite remains the same. The difference is that when codegen visits the substituted expression, it returns a TypedValue instead of a buffer index. Field access (`%.freq`) resolves via TypedValue's type rather than probing maps.

Example flow:
```
pat("c4 e4") |> transport(%, trigger(2))
```
1. Analyzer rewrites to: `transport(pat("c4 e4"), trigger(2))`
2. Codegen visits `pat("c4 e4")` → returns `TypedValue{Pattern, ...}`
3. Codegen visits `transport(...)` → sees arg 0 is Pattern → compiles inner sequence, wires clock override

## Impact on Field Access

`handle_field_access()` simplifies from 5 cases to:

```
1. Visit the expression → get TypedValue
2. Switch on type:
   - Pattern: look up field in pattern fields (freq/vel/trig/gate/type)
   - Record:  look up field in record map
   - Other:   type error
```

The current ad-hoc maps (`record_fields_`, `polyphonic_fields_`) are subsumed by the TypedValue payload.

## Impact on Builtins

`BuiltinInfo` gains type annotations:

```cpp
struct BuiltinInfo {
    cedar::Opcode opcode;
    uint8_t input_count;
    uint8_t optional_count;
    bool requires_state;
    std::array<std::string_view, MAX_BUILTIN_PARAMS> param_names;
    std::array<ValueType, MAX_BUILTIN_PARAMS> param_types;    // NEW
    std::array<float, MAX_BUILTIN_DEFAULTS> defaults;
    std::string_view description;
    uint8_t extended_param_count = 0;
};
```

`param_types` array marks each parameter's expected type. `Signal` is the default (backward compatible — any audio-rate value). Special values:

- `Signal` — accepts Signal, Number (auto-promoted to constant buffer), or Pattern (uses `.buffer` = freq)
- `Pattern` — requires Pattern typed value
- `String` — requires compile-time string literal
- `Number` — requires compile-time constant

Type checking happens in `visit_call()` after visiting each argument:
```cpp
for (int i = 0; i < num_args; i++) {
    TypedValue arg = visit(arg_node);
    ValueType expected = builtin.param_types[i];
    if (!type_compatible(arg.type, expected)) {
        error("argument '{}' expects {}, got {}", builtin.param_names[i], expected, arg.type);
    }
}
```

### Type compatibility rules

| Expected | Accepts |
|----------|---------|
| Signal | Signal, Number (promoted), Pattern (uses .buffer) |
| Pattern | Pattern only |
| Record | Record, Pattern (Pattern is a Record subtype) |
| Array | Array only |
| String | String only |
| Number | Number only |

## Migration Strategy

### Phase 1: Introduce TypedValue alongside uint16_t

- Define `TypedValue` struct in `codegen.hpp`
- Add `TypedValue visit_typed(NodeIndex)` that wraps existing `visit()` + metadata lookups
- New code can call `visit_typed()` to get type info
- `visit()` continues to return `uint16_t` unchanged
- Zero risk: existing behavior unchanged

### Phase 2: Convert visit() to return TypedValue

- `visit()` return type changes to `TypedValue`
- Each `visit_*` method returns `TypedValue` instead of `uint16_t`
- Add `buffer_of(TypedValue)` helper for callers that just need the buffer index
- Remove `node_buffers_` cache — replaced by TypedValue cache
- Remove `record_fields_`, `polyphonic_fields_` — data lives in TypedValue
- Remove `multi_buffers_`, `array_lengths_` — data lives in Array TypedValue
- Keep `stereo_outputs_` for now (stereo is orthogonal to type — a Signal can be stereo)

### Phase 3: Add type annotations to builtins

- Add `param_types` to `BuiltinInfo`
- Implement type checking in `visit_call()`
- Emit clear error messages with source locations

### Phase 4: Leverage types for new features

- `transport()` checks arg 0 is Pattern
- Builtin overload resolution based on argument types
- Better error messages everywhere

## Key Files

| File | Change |
|------|--------|
| `akkado/include/akkado/codegen.hpp` | TypedValue struct, visit() signature, remove ad-hoc maps |
| `akkado/src/codegen.cpp` | All visit_* return TypedValue, simplified field access |
| `akkado/src/codegen_patterns.cpp` | Pattern codegen returns Pattern TypedValue |
| `akkado/include/akkado/builtins.hpp` | param_types on BuiltinInfo |
| `akkado/src/codegen_builtins.cpp` | Type checking in visit_call() |

## Non-Goals

- **User-defined types.** The type system is fixed to the seven built-in types. Users cannot define new types or interfaces.
- **Type inference across functions.** Akkado has no user-defined functions — all "functions" are builtins. No need for Hindley-Milner or similar.
- **Runtime type tags.** Types exist only at compile time. No runtime type checking — the Cedar VM is untyped.
- **Stereo as a type.** Stereo is a property of Signal, not a separate type. `stereo_outputs_` may become a flag on TypedValue in the future.
- **Changing the Cedar VM.** This is purely a compiler change. Same opcodes, same instruction format, same bytecode output.

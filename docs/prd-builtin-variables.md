# PRD: Builtin Variables (`bpm`, `sr`)

## Context

`bpm = 120` appears in the default editor code, tutorials, language spec, and examples — but it does nothing. It compiles as a regular variable assignment (stores 120 in a buffer). The actual BPM is controlled externally via the Transport UI → AudioWorklet → WASM → `VM::set_bpm()`. We need to make `bpm = 120` actually set the tempo, and add the general concept of "builtin variables" (readable/writable identifiers backed by VM state).

## Approach: Compile-time metadata + runtime read opcode

**Writes (`bpm = 120`)**: The compiler intercepts assignment to builtin variables, evaluates the RHS as a compile-time constant via `ConstEvaluator`, and stores the value as metadata in `CodeGenResult`. The host applies the value when loading the program.

**Reads (`x = 60 / bpm`)**: A new opcode (`GETVAR_BPM`) fills an output buffer with the current `ctx.bpm` value, allowing `bpm` to be used in expressions.

**Two builtin variables initially**: `bpm` (read-write) and `sr` (read-only).

## Changes

### 1. Cedar VM — New opcodes

**`cedar/include/cedar/vm/instruction.hpp`** — Add two opcodes in an unused range (e.g., 190-191):
```cpp
// Builtin variable access (190-199)
GETVAR_BPM = 190,   // Fill output buffer with ctx.bpm
GETVAR_SR = 191,     // Fill output buffer with ctx.sample_rate
```

**`cedar/include/cedar/opcodes/` — New file `builtins.hpp`** (or add to `sequencing.hpp`):
```cpp
inline void op_getvar_bpm(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = ctx.bpm;
}
inline void op_getvar_sr(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = ctx.sample_rate;
}
```

**`cedar/src/vm/vm.cpp`** — Add dispatch cases in the main switch.

### 2. Akkado Compiler — Symbol table

**`akkado/include/akkado/symbol_table.hpp`**:

Add new enum value:
```cpp
enum class SymbolKind : std::uint8_t {
    Variable,
    Builtin,
    BuiltinVariable,  // <-- NEW
    Parameter,
    // ...
};
```

Add info struct:
```cpp
struct BuiltinVarInfo {
    cedar::Opcode read_opcode;  // Opcode to emit when reading
    bool writable;              // true for bpm, false for sr
    float default_value;        // For metadata (120.0 for bpm)
    float min_value;            // Validation
    float max_value;            // Validation
};
```

Add field to `Symbol`:
```cpp
BuiltinVarInfo builtin_var;  // Only valid if kind == BuiltinVariable
```

**`akkado/src/symbol_table.cpp`** — Register in `register_builtins()`:
```cpp
// Builtin variables
{
    Symbol sym{};
    sym.kind = SymbolKind::BuiltinVariable;
    sym.name = "bpm";
    sym.name_hash = fnv1a_hash("bpm");
    sym.buffer_index = 0xFFFF;
    sym.builtin_var = {cedar::Opcode::GETVAR_BPM, true, 120.0f, 1.0f, 999.0f};
    define(sym);
}
{
    Symbol sym{};
    sym.kind = SymbolKind::BuiltinVariable;
    sym.name = "sr";
    sym.name_hash = fnv1a_hash("sr");
    sym.buffer_index = 0xFFFF;
    sym.builtin_var = {cedar::Opcode::GETVAR_SR, false, 48000.0f, 0.0f, 0.0f};
    define(sym);
}
```

### 3. Akkado Compiler — Analyzer

**`akkado/src/analyzer.cpp`** in `collect_definitions()` (~line 189):

Before the immutability check, skip assignments to writable builtin variables:
```cpp
if (n.type == NodeType::Assignment) {
    const std::string& name = n.as_identifier();
    
    // Allow assignment to writable builtin variables (bpm)
    auto existing = symbols_.lookup(name);
    if (existing && existing->kind == SymbolKind::BuiltinVariable) {
        if (!existing->builtin_var.writable) {
            error("E170", "Cannot assign to read-only builtin '" + name + "'", n.location);
        }
        // Don't re-register as Variable — skip the rest of collect_definitions for this node
        return; // (or continue to next sibling)
    }
    
    // Existing immutability check...
    if (symbols_.is_defined_in_current_scope(name)) {
        error("E150", "Cannot reassign immutable variable '" + name + "'", n.location);
    }
    // ... rest of existing code
```

### 4. Akkado Compiler — Codegen

**`akkado/include/akkado/codegen.hpp`** — Add metadata struct and field:
```cpp
struct BuiltinVarOverride {
    std::string name;
    float value;
    SourceLocation location;
};
```

Add to `CodeGenResult`:
```cpp
std::vector<BuiltinVarOverride> builtin_var_overrides;
```

Add to `CodeGenerator` private members:
```cpp
std::vector<BuiltinVarOverride> builtin_var_overrides_;
```

**`akkado/src/codegen.cpp`** — Two changes:

**Identifier case** (~line 421): Add before the existing `Variable`/`Parameter` case or after it:
```cpp
if (sym->kind == SymbolKind::BuiltinVariable) {
    std::uint16_t out = buffers_.allocate();
    cedar::Instruction inst = cedar::Instruction::make_nullary(
        sym->builtin_var.read_opcode, out);
    emit(inst);
    return cache_and_return(node, TypedValue::signal(out));
}
```

**Assignment case** (~line 510): Add early check before the pattern check:
```cpp
auto sym = symbols_->lookup(var_name);
if (sym && sym->kind == SymbolKind::BuiltinVariable) {
    if (!sym->builtin_var.writable) {
        error("E170", "Cannot assign to read-only builtin variable '" + var_name + "'", n.location);
        return TypedValue::error_val();
    }
    // Evaluate RHS as compile-time constant
    ConstEvaluator evaluator(*ast_, *symbols_);
    auto const_val = evaluator.evaluate(value_idx);
    if (const_val) {
        if (auto* scalar = std::get_if<double>(&*const_val)) {
            builtin_var_overrides_.push_back({var_name, static_cast<float>(*scalar), n.location});
            return cache_and_return(node, TypedValue::void_val());
        }
        error("E171", "Builtin variable '" + var_name + "' requires a scalar value", n.location);
        return TypedValue::error_val();
    }
    error("E172", "'" + var_name + "' must be assigned a constant expression (e.g., bpm = 120)", n.location);
    return TypedValue::error_val();
}
```

**`generate()` function**: Wire `builtin_var_overrides_` into the result, clear on init.

### 5. Akkado top-level — CompileResult

**`akkado/include/akkado/akkado.hpp`** — Add to `CompileResult`:
```cpp
std::vector<BuiltinVarOverride> builtin_var_overrides;
```

**`akkado/src/akkado.cpp`** (~line 194): Add after param_decls move:
```cpp
result.builtin_var_overrides = std::move(gen.builtin_var_overrides);
```

### 6. WASM Bindings

**`web/wasm/enkido_wasm.cpp`** — Add exports to query overrides from `g_compile_result`:
```cpp
WASM_EXPORT uint32_t akkado_get_builtin_var_override_count();
WASM_EXPORT const char* akkado_get_builtin_var_override_name(uint32_t index);
WASM_EXPORT float akkado_get_builtin_var_override_value(uint32_t index);
```

### 7. Web — AudioWorklet

**`web/static/worklet/cedar-processor.js`**:
- Add `extractBuiltinVarOverrides()` method (mirrors `extractParamDecls` pattern)
- In `compile()`: extract overrides, include in message posted back to main thread
- In `loadCompiledProgram()`: apply `bpm` override via `this.module._cedar_set_bpm(value)` before loading

### 8. Web — Audio Store

**`web/src/lib/stores/audio.svelte.ts`**:
- In `handleWorkletMessage` for `'compiled'` case: read `builtinVarOverrides` from message
- If BPM override present, update `state.bpm` to sync the Transport UI display

### 9. Web — Transport UI (minor)

**`web/src/lib/components/Transport/Transport.svelte`**: Already reactive to `audioEngine.bpm`. May need to ensure `bpmInput` local state re-syncs when `audioEngine.bpm` changes from compilation (likely already works via existing `$derived`).

## Edge Cases

- **`bpm = 60 * 2`**: Works — `ConstEvaluator` handles arithmetic on constants
- **`bpm = param("tempo", 120, 60, 200)`**: Rejected with E172 — not a constant expression. Correct behavior (dynamic BPM from code would fight with Transport UI)
- **Multiple `bpm =` lines**: Last one wins (overrides applied in order). Could optionally warn.
- **`sr = 44100`**: Rejected with E170 — read-only
- **`x = 60 / bpm`**: Works — `bpm` emits GETVAR_BPM opcode, result used in division
- **Hot-swap**: BPM override applied in `loadCompiledProgram()` before program starts, crossfade uses new BPM for both channels (correct)
- **Removing `bpm =` from code**: BPM stays at Transport UI value (no override = no change)

## Key Files
- `cedar/include/cedar/vm/instruction.hpp` — Opcode enum
- `cedar/src/vm/vm.cpp` — VM dispatch
- `akkado/include/akkado/symbol_table.hpp` — SymbolKind, BuiltinVarInfo, Symbol
- `akkado/src/symbol_table.cpp` — register_builtins()
- `akkado/src/analyzer.cpp` — collect_definitions() immutability bypass
- `akkado/include/akkado/codegen.hpp` — BuiltinVarOverride, CodeGenResult, CodeGenerator members
- `akkado/src/codegen.cpp` — Identifier + Assignment cases
- `akkado/include/akkado/akkado.hpp` — CompileResult
- `akkado/src/akkado.cpp` — Wire overrides through
- `web/wasm/enkido_wasm.cpp` — WASM exports
- `web/static/worklet/cedar-processor.js` — Extract + apply overrides
- `web/src/lib/stores/audio.svelte.ts` — Sync Transport UI

## Verification

1. **C++ tests**: Build akkado_tests, add test cases:
   - `bpm = 120` → `builtin_var_overrides` contains `{bpm, 120.0}`
   - `x = bpm` → emits `GETVAR_BPM` instruction
   - `sr = 44100` → error E170
   - `bpm = sin(1)` → error E172 (not constant)
   - `bpm = 60 * 2` → override value is 120.0
2. **VM tests**: `GETVAR_BPM` fills buffer with ctx.bpm value
3. **Web integration**: Compile `bpm = 140`, verify Transport shows 140, patterns play at 140 BPM

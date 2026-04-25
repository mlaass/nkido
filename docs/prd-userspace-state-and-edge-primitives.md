# PRD: Userspace State Primitives and Edge-Triggered Operators

> **Status: NOT STARTED** — Adds `state` cells, edge-triggered SAH modes, and method-call codegen so users can implement state-bearing operators (e.g. `step`) without C++ changes.

## 1. Overview

This PRD introduces three small, coordinated additions to Akkado and Cedar that, together, make it possible to write **stateful userspace closures** — closures that maintain state across audio blocks. The motivating example is a `step` operator that walks an array on each rising edge of a trigger, but the same primitives unlock custom envelopes, counters, latches, and other live-coding building blocks that today require new C++ opcodes.

The three additions are:

1. **State cells (`state(init)`)** — a first-class persistent storage primitive for closures. Reads via `s.get()`, writes via `s.set(v)`. Each `state(...)` call site allocates its own state-pool slot via the existing semantic-ID-path mechanism. The builtin is named `state` to align with Cedar's existing "state pool" / `state_id` terminology.
2. **EDGE_OP opcode** — the existing `SAH` opcode renamed to reflect its broader role, with three new modes selected by `inst.rate`:
   - mode 0: classic sample-and-hold (unchanged)
   - mode 1: `gateup` — output 1.0 on rising edge of input
   - mode 2: `gatedown` — output 1.0 on falling edge of input
   - mode 3: `counter` — output increments on rising edge, with optional reset/start
3. **UFCS method-call codegen** — fill in the `MethodCall` codegen stub at `akkado/src/codegen.cpp:1512` so `x.foo(a, b)` lowers to `foo(x, a, b)` for any callable `foo` in scope (builtins and closures alike). The parser already accepts this syntax.

### Why?

Today, every stateful operator must be implemented as a C++ opcode in `cedar/include/cedar/opcodes/` because Akkado closures regenerate their bytecode each block — local bindings do not survive block boundaries. This PRD ends that limitation: users write state-bearing operators directly in Akkado and ship them in `.akk` patches. The three additions are the minimum surface area needed: state cells provide cross-block persistence, edge primitives provide trigger detection, and UFCS makes the call sites read naturally (`arr.step(trig)`).

### Major design decisions

- **State cells are the persistence primitive**, not language-level `static` declarations or mutable records. State cells are explicit, composable, and add a single new value type without altering the immutability of records or introducing a new keyword.
- **No new opcodes are added.** EDGE_OP is the renamed SAH opcode; state cell load/store reuse existing scalar instructions plus a small extension to the codegen pipeline that emits state-pool sync points around closure bodies. Mode dispatch on EDGE_OP follows the established `inst.rate` field convention from CLAUDE.md §Extended Parameter Patterns.
- **Method calls desugar universally** — no type-dispatch table, no method tables on records. `x.foo(a)` ≡ `foo(x, a)` if `foo` is callable. Same model as Rust UFCS.

---

## 2. Problem Statement

### What exists today

| Capability | Status | Reference |
|---|---|---|
| Array literals `[1,2,3,4]` | ✅ Works | `akkado/src/parser.cpp:609`, `codegen.cpp:222` |
| Dynamic indexing `arr[i]` | ✅ Works | `ARRAY_INDEX` opcode, `instruction.hpp:161` |
| Multi-statement closure bodies | ✅ Works | `parser.cpp:882`, `codegen.cpp:893` |
| Reassignment within a block | ✅ Works (silent upsert) | `codegen.cpp:568` |
| Method-call **parsing** | ✅ Works | `parser.cpp:990` |
| Method-call **codegen** | ❌ Throws E113 | `codegen.cpp:1512` |
| Sample-and-hold opcode | ✅ Works | `cedar/include/cedar/opcodes/utility.hpp:185` |
| `select`, `wrap`, `clamp`, `len`, `mod` | ✅ Work | `instruction.hpp:46-47, 140`; builtin table |
| Cross-block persistence in closures | ❌ No mechanism | — |
| Standalone edge-detection builtins | ❌ Don't exist | — |

### What's missing

To write `step` as a userspace closure, a user must be able to (1) detect rising edges of a trigger signal, (2) hold a piece of state across audio blocks, and (3) call the operator with method-style syntax. None of those three are currently available. The compiler accepts `arr.step(trig)` as a parse but the codegen explicitly errors with `E113: Method calls not supported in MVP`. The state-pool / `state_id` mechanism that backs every stateful built-in is not exposed to user code in any form.

### Current vs proposed

| Today | After this PRD |
|---|---|
| Stateful operators must be implemented in C++ | Stateful operators can be implemented in Akkado |
| `arr.step(trig)` → compile error | `arr.step(trig)` → `step(arr, trig)` |
| No way to "remember" a value between blocks in user code | `state(init)` + `.get()` / `.set()` |
| Edge detection is implicit inside specific opcodes (ADSR, AR, SAH, SAMPLE_PLAY) | Standalone `gateup(sig)`, `gatedown(sig)` builtins |
| Counter / accumulator only available via `SEQPAT_TRANSPORT` (internal) | `counter(trig, reset, start)` builtin |

---

## 3. Goals and Non-Goals

### Goals

1. **Ship `step` as userspace Akkado**. All four variants — `step(arr, trig)`, `step(arr, trig, reset)`, `step(arr, trig, reset, start)`, `step_dir(arr, trig, dir)` — implementable in `.akk` files using only the primitives this PRD introduces.
2. **Add `state(init)` cells** as a first-class value type with `.get()` and `.set(v)` operations. State cells use the existing state-pool / semantic-ID-path mechanism for cross-block persistence. Each `state(...)` call site gets its own slot.
3. **Rename `SAH` opcode to `EDGE_OP`** and add three modes via `inst.rate`: classic hold (rate 0, current behavior), `gateup` (rate 1), `gatedown` (rate 2), `counter` (rate 3). The existing `sah` builtin keeps working unchanged (lowers to `EDGE_OP rate=0`).
4. **Add new builtins**: `gateup(sig)`, `gatedown(sig)`, `counter(trig, reset, start)` (with reset/start optional via the default-constants pattern).
5. **Implement method-call codegen** as universal UFCS desugaring at `akkado/src/codegen.cpp:1512`. Works for any callable in scope — builtins and closures.
6. **Zero regressions** in existing patches and tests. Existing `sah(in, trig)` calls continue to compile and produce identical bytecode (modulo opcode renaming).

### Non-Goals

- **No `static` keyword.** State cells subsume that use case and avoid reserving a new identifier.
- **No record field mutation.** Records remain immutable values. (Could be revisited in a future PRD if needed.)
- **No type dispatch on method calls.** UFCS only — `x.foo(a)` always means `foo(x, a)`. Records do not have method tables.
- **No type checking on `set()` value.** The state cell holds a single float; passing a non-float is undefined for now (matches existing builtin laxness).
- **No multi-element state cells.** Cells hold one float. Cells of arrays / records are out of scope.
- **No reactive / change-tracking semantics.** `set()` is immediate; there are no observers, no diff propagation. A state cell is a slot, not a stream.
- **No new opcodes.** EDGE_OP is a rename of an existing opcode. State cell `.get()` / `.set()` reuse scalar move infrastructure plus state-pool sync (see §6).
- **No PHP-style late binding for method calls.** UFCS resolves `foo` lexically at the call site.

---

## 4. Target Syntax

### 4.1 Edge-detection builtins

```akkado
// trigger() emits beat-division pulses; gateup detects rising edges of any signal
trig = trigger(4)        // 16th-note pulse train
hits = gateup(trig)      // 1.0 on the sample where trig goes 0 → 1, else 0.0

// gatedown for falling edges, useful for note-off events
release = gatedown(gate_signal)
```

### 4.2 Counter

```akkado
// Bare counter: increments by 1 on each rising edge of trig, never resets
n = counter(trigger(1))

// With reset: counter returns to 0 on rising edge of reset signal
n = counter(trigger(4), beat(4))      // resets every 4 beats

// With reset and explicit start value
n = counter(trigger(4), beat(4), 7)   // resets to 7 instead of 0
```

### 4.3 State cells

```akkado
// Top-level state: one stable slot
my_idx = state(0)

my_idx.get()              // read current value
my_idx.set(42)            // write new value (returns the new value, can be ignored)

// State inside a closure: each call site gets its own slot via semantic-ID hashing
make_counter = () -> {
  s = state(0)
  () -> { s.set(s.get() + 1); s.get() }   // returns a fresh counter closure each call
}
```

### 4.4 The `step` family in userspace

The simplest form (no reset, no start) using the new `counter` builtin:

```akkado
step = (arr, trig) -> arr[counter(trig) % len(arr)]

// Usage
[60, 64, 67, 72].step(trigger(4)) |> note(%) |> osc("sin", %) |> out(%, %)
```

With reset:

```akkado
step = (arr, trig, reset) -> arr[counter(trig, reset) % len(arr)]
```

With reset and start (the full form):

```akkado
step = (arr, trig, reset, start) -> arr[counter(trig, reset, start) % len(arr)]
```

A directional variant using a state cell directly:

```akkado
step_dir = (arr, trig, dir) -> {
  idx = state(0)
  idx.set(select(
    gateup(trig),
    wrap(idx.get() + dir, 0, len(arr)),
    idx.get()
  ))
  arr[idx.get()]
}

// Forward/backward arpeggiator
[60, 64, 67, 72].step_dir(trigger(4), 1)    // up
[60, 64, 67, 72].step_dir(trigger(4), -1)   // down
```

### 4.5 Method-call universality

UFCS works for any callable in scope:

```akkado
// Existing builtins
sig.lp(1200)          // ≡ lp(sig, 1200)
sig.delay(0.3, 0.5, 0.5, 0.5)

// User closures
my_func = (x, a) -> x * a + 1
val.my_func(2)        // ≡ my_func(val, 2)

// Pipe-and-method are interchangeable
sig |> lp(%, 1200)    // existing
sig.lp(1200)          // new equivalent
```

---

## 5. Architecture

### 5.1 State cells

A state cell is a new `TypedValue` kind that carries a 32-bit `state_id` (computed from the AST position of the `state(...)` call site, same hashing scheme as builtins). The cell value is a compile-time handle; at runtime it corresponds to a single `float` slot in the state pool.

**Compile-time representation:**

```cpp
// in akkado/include/akkado/typed_value.hpp (extended)
struct TypedValue {
  enum class Kind { Scalar, Array, Record, Pattern, Closure, StateCell };  // StateCell added
  // ...
  std::uint32_t cell_state_id;  // populated when Kind == StateCell
};
```

**Runtime state:**

```cpp
// in cedar/include/cedar/dsp_state.hpp
struct CellState {
  float value;
  bool initialized;
};
```

`CellState` is added to the state-pool variant alongside `OscState`, `EdgeState` (renamed `SAHState`), etc. (Internal name keeps the `FooState` convention used throughout the pool — `CellState` means "state pool entry for a state cell".)

**`state(init)` codegen:**

1. Compute `state_id` from the AST node of the `state` call (semantic-ID path hash, same as existing builtins).
2. Emit a `CELL_INIT` instruction (see §5.2 for why this isn't a "new opcode") that, on first execution per state slot, writes `init` into the slot. On subsequent invocations it's a no-op.
3. Return a `TypedValue { Kind::StateCell, cell_state_id = <hash> }`.

**`s.get()` codegen:**

1. UFCS desugar: `s.get()` → `get(s)`.
2. The `get` builtin is intrinsic — it receives the cell's `state_id` at compile time and emits a `CELL_LOAD` instruction that copies the slot's `float` into a fresh per-sample buffer (broadcast as a constant across the block).
3. Type error if the argument isn't a state cell.

**`s.set(v)` codegen:**

1. UFCS desugar: `s.set(v)` → `set(s, v)`.
2. The `set` builtin is intrinsic — emits a `CELL_STORE` instruction that takes the **last sample** of `v`'s buffer and writes it to the cell's slot (per-sample writes would be wasteful and is not the semantic we want — state cells are scalar registers, not streams).
3. Returns the new value as a (constant-broadcast) buffer so `set()` can be used in expression position.

> **Note on "no new opcodes":** `CELL_INIT`, `CELL_LOAD`, `CELL_STORE` are presented as logical operations. Implementation-wise they are folded into existing opcode infrastructure: each is a thin variant of MOV / SCALAR opcodes that touches the state pool. **[OPEN QUESTION — see §10]** If folding proves impossible without a true new opcode, we add the minimal one needed (one or two opcodes), and document why the no-new-opcodes goal slipped.

### 5.2 EDGE_OP (renamed from SAH)

The existing `SAH` opcode handler at `cedar/include/cedar/opcodes/utility.hpp:185-199` is renamed to `op_edge` (file → `edge_op.hpp`). The `Opcode::SAH` enum entry in `instruction.hpp:106` is renamed to `Opcode::EDGE_OP`. Since opcode metadata is auto-generated (`web/scripts/build-opcodes.ts` per CLAUDE.md), the rename propagates everywhere with one regeneration.

The handler dispatches on `inst.rate`:

```cpp
inline void op_edge(ExecutionContext& ctx, const Instruction& inst) {
  auto& state = ctx.states->get_or_create<EdgeState>(inst.state_id);
  float* out = ctx.buffers->get(inst.out_buffer);

  switch (inst.rate) {
    case 0: { // classic SAH (in, trig) — unchanged behavior
      const float* input = ctx.buffers->get(inst.inputs[0]);
      const float* trig = ctx.buffers->get(inst.inputs[1]);
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (state.prev_trig <= 0.0f && trig[i] > 0.0f) state.held = input[i];
        state.prev_trig = trig[i];
        out[i] = state.held;
      }
      break;
    }
    case 1: { // gateup(sig) — output 1.0 on rising edge of inputs[0]
      const float* sig = ctx.buffers->get(inst.inputs[0]);
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (state.prev_trig <= 0.0f && sig[i] > 0.0f) ? 1.0f : 0.0f;
        state.prev_trig = sig[i];
      }
      break;
    }
    case 2: { // gatedown(sig) — output 1.0 on falling edge
      const float* sig = ctx.buffers->get(inst.inputs[0]);
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (state.prev_trig > 0.0f && sig[i] <= 0.0f) ? 1.0f : 0.0f;
        state.prev_trig = sig[i];
      }
      break;
    }
    case 3: { // counter(trig, reset, start) — increment on trig edge, reset on reset edge
      const float* trig  = ctx.buffers->get(inst.inputs[0]);
      const float* reset = (inst.inputs[1] != 0xFFFF) ? ctx.buffers->get(inst.inputs[1]) : nullptr;
      const float* start = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
      for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // reset wins if both fire on the same sample
        if (reset && state.prev_reset <= 0.0f && reset[i] > 0.0f) {
          state.held = start ? start[i] : 0.0f;
        } else if (state.prev_trig <= 0.0f && trig[i] > 0.0f) {
          state.held = state.held + 1.0f;
        }
        state.prev_trig = trig[i];
        if (reset) state.prev_reset = reset[i];
        out[i] = state.held;
      }
      break;
    }
  }
}
```

`SAHState` is renamed to `EdgeState` and gains a `prev_reset` field (used only in counter mode):

```cpp
struct EdgeState {
  float held = 0.0f;
  float prev_trig = 0.0f;
  float prev_reset = 0.0f;  // new — counter mode only
};
```

### 5.3 UFCS method-call codegen

Replace the stub at `akkado/src/codegen.cpp:1511-1513` with a desugaring pass:

```cpp
case NodeType::MethodCall: {
  // Children: [receiver, arg1, arg2, ...] (per parser.cpp:1008-1017)
  // Method name is in n.name_index (string-interned)
  // Desugar to: name(receiver, arg1, arg2, ...)
  //
  // Implementation: rewrite as if the user had written a Call node
  // with the method name as the callee and receiver prepended to args.
  return visit_call(ast_, node, /*callee_name=*/n.name_index, /*prepend_receiver=*/true);
}
```

Existing call codegen (`visit_call`) already handles both builtin and closure callees uniformly — the only thing missing is the receiver-prepending entry point, which is straightforward.

If the named callable doesn't exist in scope, raise a clear error: `"E113: Method '{name}' not found — '{name}' must be a builtin or closure in scope"`.

### 5.4 How the userspace `step` works end-to-end

User code:

```akkado
step = (arr, trig) -> arr[counter(trig) % len(arr)]
[60, 64, 67, 72].step(trigger(4))
```

Compile flow:

1. `step = (arr, trig) -> arr[counter(trig) % len(arr)]` defines a closure in the symbol table.
2. `[60, 64, 67, 72].step(trigger(4))` parses as a MethodCall on an ArrayLit.
3. UFCS desugar: `step([60,64,67,72], trigger(4))`.
4. Closure expansion: codegen visits the closure body with `arr` bound to the array buffer and `trig` bound to the trigger buffer.
5. `counter(trig)` resolves to `EDGE_OP rate=3 inputs=[trig, ∅, ∅]`. Its `state_id` is hashed from the AST path of the `counter` call inside this specific call site of `step` — so a second `step(...)` invocation with a different array gets a different counter slot.
6. `% len(arr)` lowers to MOD with `len([60,64,67,72]) = 4` (compile-time known for literal arrays).
7. `arr[...]` lowers to `ARRAY_INDEX` with wrap mode.

No language-level state-tracking is needed for the closure body — the `counter` builtin (via EDGE_OP) carries the persistent state. For the `step_dir` variant, the state cell created via `state(0)` is what carries the index across blocks.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar` opcode list (`instruction.hpp`) | **Modified** | `SAH` → `EDGE_OP` rename. No additions. |
| `SAH` opcode handler | **Modified → Renamed** | File rename + multi-mode dispatch on `inst.rate` |
| `SAHState` | **Renamed** | → `EdgeState`, gains `prev_reset` field |
| Existing `sah` builtin | **Stays** | Lowers to `EDGE_OP rate=0` — bytecode-compatible |
| `gateup`, `gatedown`, `counter` builtins | **New** | Entries in `akkado/include/akkado/builtins.hpp` |
| `state` builtin | **New** | Returns a StateCell typed value |
| `get`, `set` intrinsics | **New** | Recognized by codegen for StateCell args |
| `TypedValue` | **Modified** | Adds `Kind::StateCell` and `cell_state_id` |
| `CellState` | **New** | Added to state-pool variant |
| MethodCall codegen (`codegen.cpp:1511`) | **Modified** | Stub → UFCS desugar |
| Auto-generated opcode metadata | **Regenerated** | `bun run build:opcodes` after rename |
| `web/wasm/nkido_wasm.cpp` | **Stays** | Uses generated opcode metadata; rebuild only |
| `tools/nkido-cli/bytecode_dump.cpp` | **Stays** | Same |
| Existing tests | **Stays** | Should pass with no changes; SAH→EDGE_OP propagates via metadata |
| Existing patches (`web/static/patches/*.akk`) | **Stays** | `sah(in, trig)` still works |
| Documentation (`web/static/docs/`) | **Modified** | New entries for state cells, gateup, gatedown, counter, methods |

---

## 7. File-Level Changes

### Modify

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Rename `Opcode::SAH` → `Opcode::EDGE_OP` |
| `cedar/include/cedar/opcodes/utility.hpp` | Move `op_sah` to a new file (next row), leave a forwarding include if needed |
| `cedar/include/cedar/dsp_state.hpp` | Rename `SAHState` → `EdgeState`; add `prev_reset` |
| `cedar/src/vm/dispatch.cpp` (or wherever opcode switch lives) | Update to reference `EDGE_OP` |
| `akkado/include/akkado/builtins.hpp` | Update `sah` entry to use `EDGE_OP rate=0`; add `gateup`, `gatedown`, `counter`, `state`, `get`, `set` entries |
| `akkado/include/akkado/typed_value.hpp` | Add `Kind::StateCell`, `cell_state_id` field |
| `akkado/src/codegen.cpp` | Replace MethodCall stub at line 1511 with UFCS desugar; add StateCell-aware codegen for `state`/`get`/`set` |
| `web/static/docs/reference/builtins/` | New docs pages for state cells, edge primitives, counter |

### Create

| File | Purpose |
|---|---|
| `cedar/include/cedar/opcodes/edge_op.hpp` | New home for `op_edge` (the renamed/extended SAH handler) |
| `web/static/docs/reference/builtins/state.md` | Reference for `state`, `get`, `set` |
| `web/static/docs/reference/builtins/edge.md` | Reference for `gateup`, `gatedown`, `counter` |
| `web/static/docs/reference/language/methods.md` | Reference for UFCS method calls |
| `web/static/patches/stepper-demo.akk` | Demo patch using userspace `step` |
| `akkado/tests/test_methods.cpp` | UFCS regression tests |
| `akkado/tests/test_state.cpp` | State cell semantics tests |
| `cedar/tests/test_edge_op.cpp` | Multi-mode EDGE_OP tests (replacing or augmenting `test_op_sah`) |
| `experiments/test_op_edge.py` | Python opcode test for all four EDGE_OP modes |

### Stays — explicitly no change

| File | Reason |
|---|---|
| `cedar/include/cedar/opcodes/sequencing.hpp` | TRIGGER, EUCLID, etc. unaffected |
| `web/static/patches/chord-stab.akk` | No SAH usage |
| Any patch using `sah(in, trig)` | Continues to compile and run identically |

---

## 8. Implementation Phases

### Phase 1 — EDGE_OP rename and modes

**Status**: TODO
**Goal**: Land the opcode rename and three new modes with no other language changes. Existing `sah` continues to work.

Files to modify:
- `cedar/include/cedar/vm/instruction.hpp` — rename enum
- `cedar/include/cedar/opcodes/utility.hpp` — remove `op_sah`
- `cedar/include/cedar/opcodes/edge_op.hpp` — new, contains `op_edge` with mode dispatch
- `cedar/include/cedar/dsp_state.hpp` — `SAHState` → `EdgeState`
- `cedar/src/vm/dispatch.cpp` — update opcode-to-handler mapping
- `akkado/include/akkado/builtins.hpp` — keep `sah` (rate=0); add `gateup` (rate=1, 1 input), `gatedown` (rate=2, 1 input), `counter` (rate=3, 1+2 optional inputs)

- [ ] Rename opcode enum and state struct
- [ ] Implement multi-mode handler with the four cases
- [ ] Update builtin table entries
- [ ] Run `cd web && bun run build:opcodes` to regenerate metadata
- [ ] All existing tests pass with no test code changes
- [ ] New cedar tests for each EDGE_OP mode
- [ ] New `experiments/test_op_edge.py` covering all four modes

### Phase 2 — UFCS method-call codegen

**Status**: TODO
**Goal**: Replace the E113 stub at `codegen.cpp:1512` with a working UFCS desugar.

Files to modify:
- `akkado/src/codegen.cpp` — MethodCall case

- [ ] Implement desugar: rewrite MethodCall as Call with receiver prepended
- [ ] Resolve callee in current scope (builtins + closures)
- [ ] Helpful error when callee is missing
- [ ] Tests: `arr.lp(1200) ≡ lp(arr, 1200)`, `val.my_closure(arg) ≡ my_closure(val, arg)`
- [ ] Mixed pipe + method tests: `sig.lp(1200) |> hp(%, 200)` — verify both lower correctly
- [ ] Update `web/static/docs/reference/language/methods.md`

### Phase 3 — State cells (`state`, `get`, `set`)

**Status**: TODO
**Goal**: Add the StateCell typed value and the three intrinsics.

Files to modify:
- `akkado/include/akkado/typed_value.hpp` — `Kind::StateCell` + `cell_state_id`
- `akkado/include/akkado/builtins.hpp` — `state`, `get`, `set` entries (intrinsic flag)
- `akkado/src/codegen.cpp` — StateCell-aware codegen
- `cedar/include/cedar/dsp_state.hpp` — `CellState` struct, register in state-pool variant
- `cedar/src/vm/...` — handlers for cell load/init/store (or extension of existing scalar move opcodes — see §10 open question)

- [ ] `CellState` registered in state-pool
- [ ] `state(init)` returns a StateCell value with hashed state_id
- [ ] `s.get()` reads the slot (UFCS desugars to `get(s)`)
- [ ] `s.set(v)` writes the slot (UFCS desugars to `set(s, v)`)
- [ ] Multiple `state(...)` call sites get distinct slots
- [ ] Closure invocations at different AST positions get distinct slots
- [ ] Type errors on `get`/`set` of non-StateCell args
- [ ] Tests in `akkado/tests/test_state.cpp`
- [ ] Update docs

### Phase 4 — Userspace `step` family + demo patch

**Status**: TODO
**Goal**: Validate the primitives by writing all four `step` variants in `.akk` and shipping a demo patch.

Files to create:
- `web/static/patches/stepper-demo.akk` — uses `step` and `step_dir`
- `web/static/docs/tutorials/` — tutorial entry on writing stateful operators

- [ ] All four `step` variants compile and produce correct output (verified by ear and by `experiments/`)
- [ ] Demo patch loads and runs in the web UI
- [ ] Docs link `step` examples to `state`/`counter`/`gateup` reference pages

### Future Work (out of scope for this PRD)

- State cells holding arrays or records (currently float-only)
- Reactive observers on state cells (`s.on_change(...)`)
- A `static` keyword as syntactic sugar over a hidden state cell, if state cells prove ergonomically heavy
- Method-style chaining for envelopes: `gate.adsr(...).vca(sig)`
- A `change(sig)` builtin (rising or falling) — trivial as EDGE_OP mode 4 if needed

---

## 9. Edge Cases

| Situation | Expected behavior | Rationale |
|---|---|---|
| `counter` reset and trig fire on the same sample | Reset wins — held value becomes `start`, increment skipped that sample | Predictable; matches typical hardware-counter behavior |
| `gateup(constant)` where constant is non-zero | Outputs 1.0 only on the very first sample after compile (rising edge from initial 0) | Matches the established `prev <= 0 && curr > 0` convention; not a special case |
| Empty array: `[].step(trig)` | Compile error from `% len(arr) = % 0`, surfaced as `E???: division by zero` or similar | `len = 0` is degenerate; user must provide a non-empty array |
| `state(init)` called inside a closure called twice in a patch | Each call site gets its own slot (different semantic-ID paths) | Matches existing builtin state_id behavior; multiple steppers don't share state |
| Same `state(...)` AST position re-visited via recursion | Same slot — recursion shares state | Matches state-pool semantics; if user wants distinct state, they call `state()` from a closure factory |
| `s.set(stream)` where `stream` varies sample-by-sample | Last sample of the block is written to the slot | State cells are scalar registers; per-sample writes would create a contention model that doesn't match the storage semantic |
| `s.get()` before any `set()` | Returns the `init` value passed to `state(...)` | `CellState.initialized` ensures this |
| Hot-swap: code reloaded with same closure structure | Cell slots preserved by semantic-ID hashing | Same as builtin state preservation |
| Hot-swap: `step` closure body changes (e.g., new operations) | Existing slot value preserved if the `state(...)` AST position is still the same | Matches existing hot-swap semantics |
| `arr.foo(args)` where `foo` is not in scope | Error: `E113: Method 'foo' not found — must be a builtin or closure in scope` | UFCS only resolves lexically |
| `arr.foo(args)` where `foo` is in scope but not callable (e.g., `foo` is a number) | Error: `E???: 'foo' is not callable` | Same error as `foo(arr, args)` would produce |
| Negative count from `counter` (impossible in mode 3 as written, but worth noting) | N/A — counter only increments. For ping-pong / decrement use `step_dir` with state cells directly | Keeps `counter` simple |
| Float overflow in `counter` after very long runs | Theoretical: `1.0f + 1.0f` per trigger; at 16th notes / 120 BPM that's ~16M+ years before float precision degrades | Not addressed in this PRD |
| `set()` used in a non-statement position | `set()` returns the new value broadcast as a buffer, so `arr[s.set(s.get()+1)]` works (advances and uses new index in one expression) | Matches expression-oriented language |

---

## 10. Open Questions

- **Q1: Is `CELL_LOAD` / `CELL_STORE` truly a "no new opcode" change?** The current scalar / MOV instructions don't read or write the state pool. Implementing state cell load/store may require either (a) extending one existing opcode to optionally touch the state pool via a flag, (b) accepting that 1–2 small new opcodes are warranted for cell I/O, or (c) folding cell ops into EDGE_OP itself with a dedicated mode. Option (c) is appealing but conflates EDGE_OP's edge-driven semantic with non-edge-driven slot I/O. **Decision needed before Phase 3.** Recommendation: accept that cell I/O is a thin instruction-level addition (CELL_LOAD / CELL_STORE) — they're trivial mechanically and the "no new opcodes" spirit was about not multiplying DSP-level opcodes, not about banning all instruction additions.

- **Q2: Should `state` accept a non-constant init?** E.g. `state(some_signal)`. The init is sampled once per slot lifetime; using a signal is meaningless for non-trivial signals (only the value at compile-evaluation time matters). Recommend: warn if `init` is not a literal or compile-time-constant expression.

- **Q3: Should `set()` return the new value or unit?** Returning the value enables expression-position use (`arr[s.set(s.get()+1)]`). Returning unit is more "command-like". Recommendation: return the new value.

- **Q4: Method-call resolution order — local closure vs builtin of the same name.** If a user defines a closure `lp` shadowing the builtin, `sig.lp(1200)` should hit the closure. Confirm this is fine and document it as a feature, not a bug.

---

## 11. Testing / Verification Strategy

### Phase 1 — EDGE_OP

Cedar tests in `cedar/tests/test_edge_op.cpp` (Catch2):

```
EDGE_OP rate=0 (classic SAH)
  - holds input value across trigger rising edges
  - identical bytecode-level output to old SAH tests

EDGE_OP rate=1 (gateup)
  - 0→1 transition produces single-sample 1.0 pulse
  - 1→1 produces 0.0 (no edge)
  - 1→0 produces 0.0 (falling edge, not rising)
  - hot-swap: state preserved across recompile

EDGE_OP rate=2 (gatedown)
  - mirror of gateup for falling edges

EDGE_OP rate=3 (counter)
  - increments by 1.0 on each rising edge of trig
  - resets to 0 (default) on rising edge of reset
  - resets to start value when start input is provided
  - reset wins if trig and reset fire on the same sample
  - held value persists across blocks
```

Python in `experiments/test_op_edge.py`: Run each mode for several blocks with synthetic input, save WAV outputs for human evaluation, assert against expected sample counts (e.g., "exactly 4 rising-edge pulses in this 1-second buffer").

### Phase 2 — UFCS

Akkado tests in `akkado/tests/test_methods.cpp`:

```
- arr.lp(1200) compiles to identical bytecode as lp(arr, 1200)
- val.my_closure(2) where my_closure = (a,b) -> a*b returns val*2
- chained: sig.lp(1200).hp(200) ≡ hp(lp(sig, 1200), 200)
- mixed pipe and method: sig.lp(1200) |> hp(%, 200) compiles
- error: undefined method name produces E113 with helpful message
- error: shadowing builtin with non-callable produces "not callable" error
```

### Phase 3 — State cells

Akkado tests in `akkado/tests/test_state.cpp`:

```
- state(0).get() returns 0 on first read
- state(0).set(5).get() returns 5 (chained)
- state cell value persists across blocks
- two distinct state() call sites have independent storage
- state() inside a closure invoked at different sites has independent storage per site
- get(non_state_cell) raises type error
- set(non_state_cell, v) raises type error
- hot-swap: state cell value preserved if AST position unchanged
```

### Phase 4 — Userspace `step` end-to-end

- Compile a patch using `step(arr, trig)` in the akkado-cli
- Audition `web/static/patches/stepper-demo.akk` in the web dev server
- Verify the pattern advances on each beat
- Verify reset variant resets at the expected boundary
- Verify `step_dir` plays forward and backward correctly
- Listen for clicks/discontinuities at array boundaries (should be inaudible — wrap is sample-accurate)

### Build / lint commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/cedar/tests/cedar_tests "[edge_op]"
./build/akkado/tests/akkado_tests "[methods]"
./build/akkado/tests/akkado_tests "[state]"

cd experiments && uv run python test_op_edge.py

cd web
bun run build:opcodes      # regenerate opcode metadata after EDGE_OP rename
bun run build:docs         # rebuild docs index after new reference pages
bun run dev                # audition stepper-demo.akk
bun run check              # type-check
```

### Acceptance criteria

- All existing tests pass with no test code changes (other than `SAH` → `EDGE_OP` rename in any direct opcode-name references in tests).
- Existing patches (any `.akk` using `sah(in, trig)`) compile and produce bit-identical output.
- All four `step` variants and `step_dir` are implementable and correct in `web/static/patches/stepper-demo.akk`.
- New reference docs link from the F1 lookup index (run `bun run build:docs`).
- No new C++ opcodes added, OR if Q1 forces 1–2 minimal cell-I/O opcodes, that decision is documented in the PRD before merge.

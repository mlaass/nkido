---
title: State Cells
category: builtins
order: 11
keywords: [state, get, set, cell, persistent, register, slot, memory, counter, accumulator, hold, store]
---

# State Cells

State cells let you persist a single floating-point value across audio blocks from inside a closure. They are the building block for writing your own stateful operators (counters, latches, custom envelopes, slot-based step sequencers, ...) directly in Akkado — without needing to add a C++ opcode.

A state cell has three operations:

- `state(init)` — allocate a cell, set its initial value
- `cell.get()` — read the current value
- `cell.set(v)` — write a new value

Each `state(...)` call site at a distinct AST position gets its own slot. Cells survive hot-swap as long as their `state(...)` AST position is structurally unchanged.

## state

**Allocate a state cell.** Returns a handle that `get` / `set` consume.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| init  | number | -       | Value written to the slot the very first time the cell is touched |

```akk
my_idx = state(0)            // a new cell starting at 0
my_idx.set(42)               // write
my_idx.get()                 // read → 42
```

The `init` value is consumed *only* on the first execution of the program; subsequent runs (including those after a hot-swap edit) preserve whatever value `set()` last wrote. To force re-initialization after editing the init expression, move the `state(...)` call to a different AST position.

`state` is a reserved identifier — you cannot define a closure named `state`.

## get

**Read the current value of a state cell.** Returns a Signal whose every sample equals the slot value.

| Param | Type      | Default | Description |
|-------|-----------|---------|-------------|
| cell  | StateCell | -       | The cell to read |

```akk
counter = state(0)
counter.get() |> mtof(%) |> sine(%) |> out(%, %)
```

`get` is a reserved identifier.

## set

**Write a new value to a state cell.** Stores the value at the *last sample* of the input buffer (state cells are scalar registers — per-sample writes would conflict with that semantic). Returns the new value as a Signal so `set` can be used in expression position.

| Param | Type      | Default | Description |
|-------|-----------|---------|-------------|
| cell  | StateCell | -       | The cell to write to |
| value | signal    | -       | The new value (last sample stored) |

```akk
// Increment a counter on every rising edge of trig
n = state(0)
n.set(n.get() + gateup(trig))

// In expression position — increment AND read in a single line
arr[n.set(n.get() + 1)]
```

`set` is a reserved identifier.

## Example: writing a stepper in userspace

```akk
notes = [60, 64, 67, 72]

// Walk the array forward on every rising edge of trig.
step = (arr, trig) -> arr[counter(trig)]

// Walk forward or backward by `dir` each tick — uses a state cell directly.
step_dir = (arr, trig, dir) -> {
  idx = state(0)
  idx.set(select(gateup(trig), idx.get() + dir, idx.get()))
  arr[idx.get()]
}

notes.step(trigger(4)) |> mtof(%) |> sine(%) |> out(%, %)
```

Related: [counter](edge.md#counter), [gateup](edge.md#gateup), [Method calls](../language/methods.md)

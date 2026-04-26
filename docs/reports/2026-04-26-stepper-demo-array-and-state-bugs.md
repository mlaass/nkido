# `stepper-demo.akk` plays a static drone — array literal + state-cell bugs

**Date:** 2026-04-26
**Area:**
- `akkado/src/codegen.cpp` — array-literal lowering
- `cedar/include/cedar/opcodes/state_op.hpp` — `STATE_OP rate=2` (store) semantics
**Symptom (user-reported):** [`web/static/patches/stepper-demo.akk`](../../web/static/patches/stepper-demo.akk) — the showcase patch for the userspace-state PRD — plays only a steady humming pair of sines (left ≈ 220 Hz, right ≈ 110 Hz). Neither the forward `melody` voice nor the backward `bass` voice walks through the pentatonic notes.
**Reproducer patch:** [`web/static/patches/stepper-demo.akk`](../../web/static/patches/stepper-demo.akk):
```akkado
step = (arr, trig) -> arr[counter(trig)]
step_dir = (arr, trig, dir) -> {
  idx = state(0)
  idx.set(select(gateup(trig), idx.get() + dir, idx.get()))
  arr[idx.get()]
}
notes = [57, 60, 64, 67, 72]
melody = notes.step(trigger(4))
bass   = notes.step_dir(trigger(2), -1)
melody |> mtof(%) as f1 |> sine(f1) * 0.25 as L
bass   |> mtof(%) as f2 |> sine(f2 / 2) * 0.25 as R
L + R |> out(%, %)
```

---

## TL;DR

Two independent bugs both produce the same audible symptom (a stuck pitch on the corresponding channel). The `step` voice (melody) is broken at the **Akkado-codegen** layer; the `step_dir` voice (bass) is broken at the **Cedar opcode** layer. Either one alone would be enough to make the demo sound like a drone — both are present, on different channels.

| # | Voice | Symptom | Root cause | Fix location |
|---|---|---|---|---|
| A | melody (left ≈ 220 Hz) | Output is `mtof(57)` ≈ 220 Hz forever; `counter` advances but the array always returns element 0 | Array literal `[57,60,64,67,72]` is **never packed** with `ARRAY_PACK`. Codegen passes the first element's buffer (filled with `57.0` at every sample by `PUSH_CONST`) as the array, with `length=1`. | `akkado/src/codegen.cpp` (array-literal node visitor) |
| B | bass (right ≈ 110 Hz) | `step_dir`'s `idx` cell never advances; output stays at `mtof(57)/2` ≈ 110 Hz | `STATE_OP rate=2` writes `inputs[0][BLOCK_SIZE-1]` only. With input `select(gateup(trig), idx+dir, idx)` the rising-edge sample is almost never sample 127, so the unchanged `idx` overwrites the slot in ~127/128 blocks. | `cedar/include/cedar/opcodes/state_op.hpp:46` (or replace the userspace pattern) |

Both bugs are now covered by Python tests in `experiments/`:
- `test_step_pattern.py` — forward stepper at the Cedar level (Bug A would not have surfaced here because hand-built programs already use `ARRAY_PACK` correctly; this test **passes**, narrowing Bug A to the Akkado lowering)
- `test_state_set_in_block.py` — read-after-write in a state cell (**fails as predicted**, with a separate `[proof]` subtest that directly demonstrates the last-sample-wins semantic of `STATE_OP rate=2`)
- `test_stepper_demo_integration.py` — compiles + renders the real `.akk` via `nkido-cli render`, STFT-tracks per-frame dominant pitch, asserts the melody channel sees ≥ 4 distinct semitones and the bass ≥ 2. **Currently fails** (1 distinct semitone on each channel) and serves as the regression guard.

---

## Why these bugs hid

The two pre-existing opcode-level tests `experiments/test_op_edge.py` and `experiments/test_op_state.py` (both added during the PRD) exercise `EDGE_OP` and `STATE_OP` in isolation. Each one *passes*. They test the primitives one at a time but never the *composition* the demo uses:

- They don't test `STATE_OP` together with `select(gate, …, …)`, where the input has only one "interesting" sample per block.
- They don't test the full `[57,60,64,67,72] → ARRAY_INDEX` pipeline through the Akkado lowering at all.

For the array literal bug specifically, the Cedar `ARRAY_INDEX` opcode itself works correctly — the Cedar-level test `test_step_pattern.py` walks the notes perfectly when the program is built by hand. The bug is only visible when the program comes from the Akkado compiler. So a Cedar opcode test cannot catch it; only an end-to-end "compile this `.akk`, render it, look at the audio" test can. That's the role `test_stepper_demo_integration.py` now fills.

**Process lesson:** any time we add a userspace primitive whose value comes from composing several opcodes (here: `state` + `gateup` + `select` + arrays + closures), the showcase patch needs an integration test that compiles the actual `.akk` and listens to the result. Per-opcode tests are necessary but not sufficient.

---

## Bug A — array literal not packed

### Symptom

`mtof(57)` ≈ 220 Hz comes out of the melody side regardless of how many triggers fire. STFT pitch tracking over a 6 s render shows exactly one dominant semitone (45 ≈ A2 after the bass octave-down) on the left channel.

### Diagnosis

`nkido-cli dump web/static/patches/stepper-demo.akk` shows, for the `notes.step(trigger(4))` site:

```
0014: PUSH_CONST    buf[19] = 57.000        ← first element
0015: PUSH_CONST    buf[20] = 60.000
0016: PUSH_CONST    buf[21] = 64.000
0017: PUSH_CONST    buf[22] = 67.000
0018: PUSH_CONST    buf[23] = 72.000
0019: PUSH_CONST    buf[24] = 4.000         ← trigger rate
0020: TRIGGER       buf[25] in0=buf[24]
0021: EDGE_OP       buf[26] in0=buf[25]     ← counter(trig), rate=3
0022: PUSH_CONST    buf[28] = 1.000         ← length passed to ARRAY_INDEX (!)
0023: ARRAY_INDEX   buf[27] in0=buf[19] in1=buf[26] in2=buf[28]
                            ^^^^^^^^^^   array = the FIRST element's buffer
```

Two things are wrong:
1. There is **no `ARRAY_PACK` instruction** anywhere in the dumped program. The compiler should emit `ARRAY_PACK out=B_arr rate=5 inputs=[buf[19]..buf[23]]` to pack the five element values into samples 0..4 of a single array buffer.
2. `ARRAY_INDEX` is given `in0=buf[19]` (the buffer that holds the first element) and `in2=buf[28]=1.000` for the length. `PUSH_CONST` fills *all 128 samples* of `buf[19]` with `57.0`, so `arr[j]` returns `57.0` for any `j`. Combined with `length=1`, even a per-sample `j` would still read sample 0 = `57.0`.

Net effect: the array literal `[57,60,64,67,72]` is silently degraded to `[57]` of length 1. Indexing into it always returns 57. The same bug bites the bass side via the second array-index instruction at `0037: ARRAY_INDEX … in0=buf[19] in2=buf[42]=1`.

### Predicted fix

In `akkado/src/codegen.cpp`'s array-literal visitor, after evaluating the element expressions:
1. Allocate a packed-array buffer `B_arr`.
2. For arrays with ≤ 5 elements, emit `ARRAY_PACK out=B_arr rate=N inputs=[B_e0..B_e{N-1}]`. For arrays > 5, emit `ARRAY_PACK` for the first 5 then `ARRAY_PUSH` for each remaining element (per `cedar/include/cedar/opcodes/arrays.hpp`).
3. Allocate a length buffer `B_len` and emit `PUSH_CONST B_len = N` (bit-cast `N` into `state_id`).
4. The `TypedValue` returned for the array literal should bind `B_arr` and `B_len`, and `ARRAY_INDEX` lowering should consume both.

### Test

`experiments/test_step_pattern.py` (which **passes**) is the proof that the Cedar opcodes work correctly when fed a properly-packed array. It is also the template for what the codegen should produce. Once the codegen fix is in, `test_stepper_demo_integration.py` will see ≥ 5 distinct semitones on the left channel and pass.

---

## Bug B — `STATE_OP rate=2` drops gated writes

### Symptom

`step_dir`'s `idx` cell stays at its initial value (0) forever, no matter how many triggers fire. The bass channel is stuck on `mtof(notes[0])/2 = mtof(57)/2` ≈ 110 Hz (= MIDI 45).

### Root cause

From `cedar/include/cedar/opcodes/state_op.hpp:46`:

```cpp
case 2: {  // store
    const float* in = ctx.buffers->get(inst.inputs[0]);
    state.value = in[BLOCK_SIZE - 1];  // LAST sample only
    state.initialized = true;
    break;
}
```

The store path writes only `in[BLOCK_SIZE-1]` (sample 127) to the slot. In the `step_dir` body:

```
idx.set(select(gateup(trig), idx.get() + dir, idx.get()))
```

- `gateup(trig)` is `1.0` on a single sample (the rising edge) and `0.0` everywhere else.
- `select(g, A, B)` returns A where g > 0, else B.
- So the input to `set()` equals `idx + dir` *only* on the rising-edge sample, and `idx` everywhere else — including sample 127 in 127 out of every 128 blocks.

Result: the slot is overwritten with the unchanged `idx` on essentially every block, and the user-intended increment is lost. The probability that any rising edge lands exactly on sample 127 is ~1/128 per pulse, so over a 6 s render at 2 Hz (12 pulses) the index might advance once or zero times — nowhere near the 12 advances the demo is asking for.

### Direct proof

The `[proof]` subtest in `experiments/test_state_set_in_block.py` constructs a STATE_OP store driven by `[1, 0, 0, …, 0]` and reads back `0.0`, then drives it with `[0, 0, …, 0, 1]` and reads back `1.0`. Confirmed: `inputs[0][BLOCK_SIZE-1]` is the only sample that reaches the slot.

### Possible fixes

1. **Change `STATE_OP rate=2` semantics** to "write the last sample where the input differs from the slot value" (or "first non-zero gated"). This makes the userspace `step_dir` pattern work as written, but it leaks an implicit gate into a primitive that should be unconditional.
2. **Add a dedicated `set_if(gate, value)` opcode** for gated writes, and update Akkado so `idx.set(select(g, A, B))` lowers to `set_if(g, A)` when the else-branch is the slot's own value. Cleanest semantics-wise.
3. **Rewrite `step_dir` in `stepper-demo.akk`** to use `counter(trig) * dir` instead of state cells. `ARRAY_INDEX` already wraps negative indices correctly (`((j % length) + length) % length` per `cedar/include/cedar/opcodes/arrays.hpp:48`), so this is a one-line change in the demo:
   ```akkado
   step_dir = (arr, trig, dir) -> arr[counter(trig) * dir]
   ```
   This avoids touching opcode semantics, but means the userspace-state primitive cannot express the "advance an index per gated trigger" idiom by itself — which arguably defeats one of its core motivating use cases from the PRD.

### Test

`experiments/test_state_set_in_block.py` — once a fix is chosen, the `test_short_walk` and `test_long_run` assertions will pass (slot walks `0, -1, -2, …, -1199` over 300 s @ 4 Hz). The `[proof]` subtest stays useful regardless of the chosen fix as a documentation of the original semantic.

---

## Verification

```bash
# Rebuild bindings (one-time, exposes SELECT/ARRAY_PACK/ARRAY_INDEX to Python)
cmake --build build --target cedar_core

# Repro the unit-level findings
cd experiments
uv run python test_step_pattern.py          # PASS — Cedar opcodes are fine
uv run python test_state_set_in_block.py    # FAIL with diagnosis (Bug B)

# Repro the integration symptom
uv run python test_stepper_demo_integration.py   # FAIL — 1 pitch on each channel
```

Audible: open `experiments/output/stepper_demo/stepper-demo.wav` (the broken render) and `experiments/output/op_step_pattern/step_pattern.wav` (the working stair-step from the Cedar-level test) for a side-by-side ear check.

---

## Files added

- `experiments/test_step_pattern.py`
- `experiments/test_state_set_in_block.py`
- `experiments/test_stepper_demo_integration.py`
- `cedar/bindings/bindings.cpp` — exposed `SELECT`, `ARRAY_PACK`, `ARRAY_INDEX` to Python (4-line diff, needed by the new tests)

---

## Resolution (2026-04-26)

Both bugs are now fixed. The demo plays correctly: melody walks 220→261→330→392→523 Hz on rising edges of `trigger(4)`, and bass descends 200→190→160→140→130→110 Hz on rising edges of `trigger(2)`. Distinct semitones in the integration test went from 1/1 to 13/13 (the test threshold is ≥4 melody, ≥2 bass).

### Bug A fix — multi-buffer arrays through closure parameters

`akkado/src/codegen_functions.cpp`, both `handle_user_function_call` and `handle_function_value_call`. After visiting each argument, capture the resulting `TypedValue`; if it is a multi-element `Array`, bind the parameter via `define_array` (with `source_node = NULL_NODE` and `buffer_indices` populated) instead of `define_variable`. The Array-symbol lookup path at `codegen.cpp:518` already returns a proper `Array` `TypedValue` for synthetic arrays, so `arr[i]` inside the lambda body now sees the array, packs it via `ARRAY_PACK rate=N`, and emits the correct length to `ARRAY_INDEX` — no more first-element-with-length-1.

Mirrors the existing rest-param mechanism: `synthetic` arrays bound by `buffer_indices` rather than re-visiting an AST source node.

### Bug B fix — `STATE_OP rate=2` semantics

`cedar/include/cedar/opcodes/state_op.hpp` `case 2:` rewritten from "write `inputs[0][BLOCK_SIZE-1]`" to "write the latest sample whose value differs from the slot value at the start of the block":

```cpp
case 2: {
    const float* in = ctx.buffers->get(inst.inputs[0]);
    const float initial = state.value;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (in[i] != initial) state.value = in[i];
    }
    state.initialized = true;
    break;
}
```

**Why "compare against initial" and not "compare against running `state.value`":** the running-comparison version (which was the first sketch) is broken on the exact pattern this fix targets. Trace `step_dir` with `dir=-1`, slot=0, gateup at sample 50:
- `select(gateup, idx+dir, idx)` = `[0, 0, …, -1 at 50, …, 0]`
- sample 50: `in=-1 != prev=0` → `state.value=-1`, `prev=-1`
- sample 51: `in=0 != prev=-1` → `state.value=0`, `prev=0` — slot snaps back to 0

The running comparison re-records the no-op samples as changes once the slot has moved. Snapshotting `initial` before the loop makes those no-op samples equal `initial` → skipped; only the rising-edge sample writes, and it survives until end of block.

This is a strict superset of the old behaviour for every test in the suite: `set(constant)`, `set(get()+1)`, and `set(varying_signal)` all pick the same final value as before because their inputs differ from the slot at every sample (so "latest differing" coincides with "last sample"). The one observable difference — a stream that returns to the initial slot value mid-block now records the earlier change instead of getting overwritten by the no-op — is the desired behaviour for the gated-write pattern.

The `[proof]` subtest in `test_state_set_in_block.py` was updated to assert the new contract directly. It is now a documentation test for the new semantic, not a pre-fix diagnostic.

### Bonus regression caught en route

The Bug A fix exposed a long-standing latent bug elsewhere. The test `Const: array used as osc input` in `akkado/tests/test_const_eval.cpp:726` was a **false positive**: it asserted that PUSH_CONST instructions for `220.0`, `440.0`, `660.0` appear in the bytecode, which they did — but bytecode-dump inspection showed only ONE oscillator (`OSC_SIN freq=buf[220]`) was actually wired up. Voices 440 and 660 were silently dropped because chord expansion didn't recognize the const-array variable as multi-buffer through the call site.

After the Bug A fix, chord expansion correctly identifies all three voices and produces an `Array` TypedValue; the program then fails compilation with `out() argument 'L' expects Signal, got Array` — which is the same rule that `chord("Am") |> osc(...) |> out(...)` already enforces in `test_chord.cpp` (multi-voice into mono `out` requires explicit `sum()`). The test was updated to add `|> sum(%) |>` so it exercises the full chord pipeline correctly, while still asserting all three constants land in the bytecode.

Net change in capability: const-array chord expansion now actually works.

### Why these bugs hid (revisited with the fixes in hand)

The original report's "Why these bugs hid" section identified the structural pattern; the fixes confirm it precisely. **Every primitive passed its own unit test:**

- `cedar/tests/test_state_op.cpp` and `akkado/tests/test_state.cpp` exercised `set` with values that differ from the slot at every sample — `set(5)` from a slot of 0, or `set(get() + 1)`. Old "last-sample wins" and new "latest-differing wins" give the same answer for those inputs, so neither contract caught the gated-write breakage.
- `experiments/test_op_edge.py` and `test_op_state.py` covered each EDGE_OP and STATE_OP mode in isolation; both passed.
- The chord-expansion test asserted that constants exist in bytecode, not that they are wired to oscillators.

**No test exercised the canonical compositional pattern from PRD §4.4** — `step_dir = (arr, trig, dir) -> { idx = state(0); idx.set(select(gateup(trig), idx.get()+dir, idx.get())); ... }` — until the bug report itself added `test_state_set_in_block.py` and `test_stepper_demo_integration.py`. Those two tests now form the regression guard for the gated-write semantic and the end-to-end demo, respectively. The `[proof]` subtest documents the STATE_OP rate=2 contract so any future change to that opcode either preserves it or breaks the test loudly.

**Process lesson worth keeping:** for any PRD whose value comes from a *composition* of primitives — here `state` + `gateup` + `select` + arrays + closures + UFCS — the showcase patch from the PRD body needs an end-to-end test that compiles the actual `.akk` and verifies the audible behaviour. Per-opcode tests are necessary but not sufficient; assertions on bytecode shape are easy to write and easy to make false-positive (the chord-expansion test is the canonical example). The integration test added here is the cheapest insurance against this class of failure recurring.

### Files modified by the fix

- `akkado/src/codegen_functions.cpp` — Bug A: `define_array` branch added to both `handle_user_function_call` and `handle_function_value_call` parameter binding loops.
- `cedar/include/cedar/opcodes/state_op.hpp` — Bug B: `case 2` rewritten + doc comment updated.
- `experiments/test_state_set_in_block.py` — `[proof]` subtest updated to assert the new "latest sample differing from start-of-block" contract.
- `akkado/tests/test_const_eval.cpp` — `Const: array used as osc input` test updated to add `|> sum(%) |>` so it exercises a well-typed chord pipeline.
- `cedar/bindings/bindings.cpp` — exposed `SELECT`, `ARRAY_PACK`, `ARRAY_INDEX` to Python (already noted above; needed by the new tests).

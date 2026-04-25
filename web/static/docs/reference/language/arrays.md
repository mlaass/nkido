---
title: Arrays
category: language
order: 5
index_headings: true
keywords: [array, list, arrays, indexing, len, length, map, reduce, fold, zipWith, zip, take, drop, reverse, sum, mean, average, rotate, shuffle, sort, normalize, scale, range, repeat, linspace, random, harmonics, polyphony, voices, multi-buffer, comprehension]
---

# Arrays

Arrays are ordered, fixed-size collections of values. They are the primary way to express **parallel signal flows** in Akkado: every element of an array becomes its own buffer, processed in parallel by the audio graph.

```akk
freqs = [220, 330, 440]
map(freqs, (f) -> osc("sin", f)) |> sum(%) * 0.3 |> out(%, %)
```

Arrays exist at compile time only — there is no runtime growable list type. `len(arr)`, `take(n, arr)`, and similar operations are evaluated when the patch is compiled, not while audio is running.

## Array Literals

Use square brackets to create an array:

```akk
// Numbers
[1, 2, 3, 4]

// Frequencies for a chord
[261.6, 329.6, 392.0]

// Mixed types are allowed
[1, "hello", true]

// Empty arrays
[]

// Nested arrays
[[1, 2], [3, 4]]
```

Trailing commas are not accepted: `[1, 2, 3,]` is a parse error.

## Indexing

Use `arr[i]` to access an element. Indices wrap modulo the array length, so out-of-range indices stay valid:

```akk
freqs = [220, 330, 440]
freqs[0]    // 220
freqs[-1]   // 440 (last element)
freqs[5]    // 440 (5 mod 3 = 2)
```

Compile-time indices (number literals) are resolved to a direct element reference with no runtime cost. Dynamic indices (e.g. an LFO selecting between voices) emit an `ARRAY_INDEX` opcode that reads the array per-sample.

```akk
// Compile-time index — zero-cost lookup
voices = [osc("sin", 220), osc("saw", 330), osc("tri", 440)]
voices[0] |> out(%, %)

// Runtime index — selects a voice from an LFO
sel = (lfo(0.5) + 1) * 1.5  // 0..3 ramp
voices[sel] |> out(%, %)
```

## Auto-Expansion via map and sum

Akkado does not auto-expand arrays into scalar functions today. To run an array of values through a synth voice, use `map` to lift the function over each element, then a reduction like `sum` to fold the parallel voices back into a single output:

```akk
// Three sine voices summed into one output
[220, 330, 440]
  |> map(%, (f) -> osc("sin", f))
  |> sum(%) * 0.3
  |> out(%, %)
```

For chord-based polyphony driven by patterns, see [poly](../builtins/sequencing#poly) — chord patterns require an explicit `poly()` wrapper to expand into per-voice signals.

## len

**Length** — Compile-time array length.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| arr   | array | -       | Input array |

Returns the number of elements as a constant signal. Errors if the argument is not a known array.

```akk
n = len([1, 2, 3, 4])  // 4
osc("sin", 220) * (1 / n) |> out(%, %)
```

## map

**Map** — Apply a unary function to each element. Each call to `fn` receives a unique semantic path, so stateful functions (oscillators, filters) get independent state per voice.

| Param | Type     | Default | Description |
|-------|----------|---------|-------------|
| array | array    | -       | Input array |
| fn    | function | -       | Closure `(x) -> ...` applied to each element |

```akk
// Three independent saw voices, summed
[110, 220, 440]
  |> map(%, (f) -> osc("saw", f) |> lp(%, 1200))
  |> sum(%) * 0.3
  |> out(%, %)
```

## reduce

**Reduce** — Reduce an array to a scalar with a binary function and an initial value. (Named `reduce` rather than `fold` because `fold` is the [wavefolding distortion](../builtins/distortion#fold).)

| Param | Type     | Default | Description |
|-------|----------|---------|-------------|
| array | array    | -       | Input array |
| fn    | function | -       | Binary closure `(acc, x) -> ...` |
| init  | signal   | -       | Initial accumulator value |

Threads `init` through `fn(init, arr[0])`, `fn(result, arr[1])`, ... in left-to-right order. Empty arrays return `init` unchanged.

```akk
// Sum-of-squares as a custom reducer
reduce([1, 2, 3, 4], (acc, x) -> acc + x * x, 0)  // 1 + 4 + 9 + 16 = 30

// Product (multiplicative reduce). Use init=1 (the multiplicative identity);
// empty arrays return 1, matching the math convention.
reduce([2, 3, 4], (a, b) -> a * b, 1)  // 24
```

## zipWith

**Zip With** — Combine two arrays element-wise with a binary function. Truncates to the shorter input.

| Param | Type     | Default | Description |
|-------|----------|---------|-------------|
| a     | array    | -       | First array |
| b     | array    | -       | Second array |
| fn    | function | -       | Binary closure `(x, y) -> ...` |

```akk
// Apply per-voice gains to a chord
freqs = [220, 330, 440]
gains = [1.0, 0.7, 0.5]
zipWith(freqs, gains, (f, g) -> osc("sin", f) * g)
  |> sum(%) * 0.3
  |> out(%, %)
```

## zip

**Zip** — Interleave two arrays into a flat array `[a[0], b[0], a[1], b[1], ...]`. Truncates to the shorter input.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| a     | array | -       | First array |
| b     | array | -       | Second array |

```akk
zip([1, 2, 3], [10, 20, 30])  // [1, 10, 2, 20, 3, 30]
```

## take

**Take** — First `n` elements. `n` must be a number literal. If `n` exceeds the array length, the entire array is returned.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| n     | literal | -       | Element count |
| array | array   | -       | Input array |

```akk
take(2, [10, 20, 30, 40])  // [10, 20]
```

## drop

**Drop** — Skip the first `n` elements; return the rest. `n` must be a number literal. If `n` ≥ length, returns an empty array.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| n     | literal | -       | Skip count |
| array | array   | -       | Input array |

```akk
drop(1, [10, 20, 30, 40])  // [20, 30, 40]
```

## reverse

**Reverse** — Reverse element order.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
reverse([1, 2, 3])  // [3, 2, 1]
```

## sum

**Sum** — Add all elements. Empty array yields a single zero. Single-element array passes through untouched.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
// Mix three voices
[osc("sin", 220), osc("sin", 330), osc("sin", 440)]
  |> sum(%) * 0.33
  |> out(%, %)
```

## mean

**Mean** — Arithmetic average. Empty array returns `0`.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
// Average detuned voices
detuned = [220, 220.5, 219.5]
center = mean(detuned)  // ~220
```

## rotate

**Rotate** — Rotate elements by `n` positions. Positive `n` rotates right, negative rotates left, and the offset wraps modulo the array length. `n` must be a number literal.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| array | array   | -       | Input array |
| n     | literal | -       | Rotation offset |

```akk
rotate([1, 2, 3, 4], 1)   // [4, 1, 2, 3]
rotate([1, 2, 3, 4], -1)  // [2, 3, 4, 1]
rotate([1, 2, 3, 4], 5)   // same as rotate(..., 1)
```

## shuffle

**Shuffle** — Deterministic Fisher-Yates permutation seeded by the call's semantic path. Two calls in the same code position always produce the same permutation; calls in different positions produce different ones.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
// Same shuffle every compile — useful for stable variations
shuffle([220, 330, 440, 550])
  |> map(%, (f) -> osc("saw", f))
  |> sum(%) * 0.25
  |> out(%, %)
```

## sort

**Sort** — Ascending numeric order. Operates at compile time on array literals of numbers; non-literal inputs pass through unchanged.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
sort([3, 1, 4, 1, 5, 9, 2, 6])  // [1, 1, 2, 3, 4, 5, 6, 9]
```

## normalize

**Normalize** — Scale elements so the minimum maps to `0` and the maximum maps to `1`. Single-element arrays return `0`.

| Param | Type  | Default | Description |
|-------|-------|---------|-------------|
| array | array | -       | Input array |

```akk
normalize([10, 20, 30])  // [0.0, 0.5, 1.0]
```

If all elements are equal, the divisor is zero — avoid `normalize` on constant-valued arrays.

## scale

**Scale** — Map elements from their current min/max to `[lo, hi]`.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| array | array  | -       | Input array |
| lo    | signal | -       | Target minimum |
| hi    | signal | -       | Target maximum |

```akk
// Map a normalized envelope shape to a filter cutoff range
shape = [0, 0.5, 1, 0.3]
cutoffs = scale(shape, 200, 4000)
```

## range

**Range** — Integer sequence `[start, start+1, ..., end-1]`. If `start > end`, the sequence counts down. Both arguments must be compile-time constants.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| start | literal | -       | Start value (inclusive) |
| end   | literal | -       | End value (exclusive) |

```akk
range(0, 4)   // [0, 1, 2, 3]
range(4, 0)   // [4, 3, 2, 1]
range(2, 2)   // []
```

## repeat

**Repeat** — Array of `n` copies of `value`. `n` must be a number literal.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| value | signal  | -       | Value to repeat |
| n     | literal | -       | Repeat count |

```akk
repeat(0.5, 4)  // [0.5, 0.5, 0.5, 0.5]
```

## linspace

**Linspace** — `n` evenly spaced values from `start` to `end`, **inclusive on both ends**. All arguments must be compile-time constants. `n=1` returns `[start]`; `n ≤ 0` returns an empty array.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| start | literal | -       | First value |
| end   | literal | -       | Last value |
| n     | literal | -       | Number of points |

```akk
linspace(0, 1, 5)      // [0, 0.25, 0.5, 0.75, 1]
linspace(100, 800, 4)  // [100, 333.33, 566.67, 800]
```

## random

**Random** — `n` deterministic random values in `[0, 1)`, seeded by the call's semantic path. Same code position → same numbers.

| Param | Type    | Default | Description |
|-------|---------|---------|-------------|
| n     | literal | -       | Number of values |

```akk
// Reproducible random voice gains
gains = random(4)
[220, 330, 440, 550]
  |> zipWith(%, gains, (f, g) -> osc("saw", f) * g)
  |> sum(%) * 0.25
  |> out(%, %)
```

## harmonics

**Harmonics** — Harmonic series `[fundamental, 2·fundamental, ..., n·fundamental]`. Includes the fundamental as the first element. Both arguments must be compile-time constants.

| Param       | Type    | Default | Description |
|-------------|---------|---------|-------------|
| fundamental | literal | -       | Base frequency |
| n           | literal | -       | Number of harmonics |

```akk
// Additive sawtooth approximation
harmonics(110, 8)
  |> map(%, (f) -> osc("sin", f) / (f / 110))
  |> sum(%) * 0.2
  |> out(%, %)
```

## Polyphony Builtins

When you need pattern-driven voice allocation rather than parallel processing of static arrays, use `poly`, `mono`, `legato`, and `spread`. They are documented in the [Sequencing reference](../builtins/sequencing).

## Common Patterns

**Parallel synthesis from a frequency list:**

```akk
[110, 165, 220, 330]
  |> map(%, (f) -> osc("saw", f) |> lp(%, f * 4))
  |> sum(%) * 0.2
  |> out(%, %)
```

**Inharmonic spectrum with linspace:**

```akk
linspace(220, 1100, 5)
  |> map(%, (f) -> osc("sin", f))
  |> sum(%) * 0.2
  |> out(%, %)
```

**Random-detuned chorus:**

```akk
base = 220
detune = scale(random(6), -5, 5)  // ±5 Hz spread
detune
  |> map(%, (d) -> osc("saw", base + d))
  |> sum(%) * 0.15
  |> out(%, %)
```

Related: [Pipes & Holes](pipes), [Closures](closures), [Math Functions](../builtins/math), [Sequencing](../builtins/sequencing)

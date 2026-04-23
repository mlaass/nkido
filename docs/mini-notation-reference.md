> **Status: REFERENCE** — User-facing syntax reference. Current and accurate.

# Mini-Notation Reference

This document provides a comprehensive reference for Akkado's mini-notation syntax, used in pattern functions like `pat()` and `timeline()`.

## Quick Reference

| Syntax | Name | Example | Description |
|--------|------|---------|-------------|
| `a b c` | Sequence | `c4 e4 g4` | Subdivide cycle equally |
| `[a b]` | Group | `[c4 e4] g4` | Explicit subdivision |
| `<a b>` | Alternation | `<c4 e4>` | Rotate per cycle |
| `[a, b]` | Polyrhythm | `[c4 e4, g4]` | Simultaneous layers |
| `{a b}%n` | Polymeter | `{c4 e4}%3` | Fixed step count |
| `*n` | Speed | `c4*2` | Repeat n times |
| `/n` | Slow | `c4/2` | Stretch duration |
| `@n` | Weight | `c4@2` | Temporal weight |
| `!n` | Repeat | `c4!3` | Replicate n times |
| `?n` | Chance | `c4?0.5` | Probability (0-1) |
| `a\|b` | Choice | `c4\|e4` | Random selection |
| `a(k,n)` | Euclidean | `bd(3,8)` | k hits in n steps |
| `~` | Rest | `c4 ~ e4` | Silent step |
| `_` | Elongate | `c4 _ e4` | Extend previous note |

## Atoms

### Pitches

Pitches follow the pattern: `note[accidental][octave]`

| Component | Values | Notes |
|-----------|--------|-------|
| Note | `a`-`g` (lowercase) or `A`-`G` (uppercase) | Case affects parsing |
| Accidental | `#` (sharp), `b` (flat) | Optional |
| Octave | `0`-`9` | Optional, defaults to 4 |

**Examples:**
```
c4      // C in octave 4 (middle C)
f#3     // F sharp in octave 3
Bb5     // B flat in octave 5
a       // A in octave 4 (lowercase defaults)
```

**Case sensitivity:**
- Lowercase (`c4`, `a3`): Always parsed as pitches
- Uppercase without octave (`A`, `C`): Parsed as chords (A major, C major)
- Uppercase with octave (`A4`, `C5`): Parsed as pitches

### Samples

Sample names are identifiers optionally followed by a variant number.

| Syntax | Example | Description |
|--------|---------|-------------|
| `name` | `bd`, `sd`, `hh` | Sample name |
| `name:n` | `bd:2`, `kick:0` | Sample with variant |

**Examples:**
```
bd          // Bass drum, default variant
sd:1        // Snare drum, variant 1
kick:2      // Kick sample, variant 2
```

### Chords

Chords start with an uppercase note letter followed by an optional accidental and quality.

| Syntax | Example | Description |
|--------|---------|-------------|
| `Root` | `C`, `Am` | Root note + quality |
| `Root#quality` | `F#m7` | With sharp |
| `Rootbquality` | `Bbmaj7` | With flat |

**Examples:**
```
C           // C major
Am          // A minor
F#m7        // F# minor 7th
Bbmaj7      // Bb major 7th
Gdim        // G diminished
Esus4       // E suspended 4th
```

### Rests

| Symbol | Description |
|--------|-------------|
| `~` | Silent step (rest) |

**Example:**
```
c4 ~ e4 ~   // C, rest, E, rest
```

### Elongation (Tidal-compatible)

| Symbol | Description |
|--------|-------------|
| `_` | Extend previous note's duration |

The `_` symbol extends the duration of the previous note to cover its time slot, following Tidal Cycles behavior.

**Example:**
```
c4 _ e4     // C takes 2/3 cycle (elongated), E takes 1/3
c4 _ _ e4   // C takes 3/4 cycle, E takes 1/4
bd _ _ sd   // bd extended, then sd
```

If `_` appears at the start (with no previous note), it has no effect.

## Grouping Constructs

### Sequence (implicit)

Elements separated by whitespace subdivide the cycle equally.

```
c4 e4 g4    // Three equal parts per cycle
```

### Group `[...]`

Explicit grouping for subdivision control.

```
[c4 e4] g4       // First half: c4+e4, second half: g4
[[c4 e4] g4] a4  // Nested grouping
```

### Alternation `<...>`

Selects one element per cycle, rotating through the list.

```
<c4 e4 g4>   // Cycle 1: c4, Cycle 2: e4, Cycle 3: g4, Cycle 4: c4...
```

### Polyrhythm `[..., ...]`

Comma-separated elements play simultaneously, each subdividing independently.

```
[c4 e4, g4 a4 b4]   // Layer 1: c4 e4, Layer 2: g4 a4 b4
```

When every comma-separated member is a bare sample atom (or rest), the
polyrhythm collapses to a single simultaneous trigger — i.e. a drum chord.
`[bd, hh]` fires the kick and hi-hat together on the same beat; `[bd, sd, hh]`
fires all three. Up to 4 voices per stack; extra voices fall back to the
older per-child interleave so nothing is silently dropped.

```
pat("[[bd, hh] hh [sd, hh] hh]")    // Standard rock groove
```

### Polymeter `{...}%n`

Fixed step count that wraps around. The `%n` specifies how many steps constitute one cycle.

```
{c4 e4 g4}%4    // 3 elements over 4 steps, wrapping
{bd sd}%3       // 2 elements over 3 steps
```

## Modifiers

Modifiers follow atoms and adjust their timing or behavior.

### Speed `*n`

Repeats the element n times within its original duration.

```
c4*2        // Play c4 twice in the time of one
c4*4        // Play c4 four times
[c4 e4]*2   // Repeat the group twice
```

### Slow `/n`

Stretches the element to span n cycles.

```
c4/2        // c4 spans 2 cycles
c4/4        // c4 spans 4 cycles
```

### Weight `@n`

Allocates temporal weight for unequal subdivisions.

```
c4@2 e4@1   // c4 gets 2/3 of the time, e4 gets 1/3
c4@3 e4     // c4 gets 3/4, e4 gets 1/4 (default weight is 1)
```

### Repeat `!n`

Replicates the element n times as separate events (default: 2).

```
c4!3        // Same as c4 c4 c4
c4!         // Same as c4 c4 (default is 2)
```

### Chance `?n`

Probability of the element playing (0-1 range, default: 0.5).

```
c4?0.5      // 50% chance of playing
c4?0.25     // 25% chance of playing
c4?         // 50% chance (default)
```

### Modifier Stacking

Multiple modifiers can be combined:

```
c4*2?0.5    // Repeat twice, each with 50% chance
c4:2@3      // Hold for 2 units with weight 3
```

## Random Choice `|`

Selects randomly between alternatives.

```
c4|e4           // Randomly play c4 or e4
c4|e4|g4        // Random choice of three
[c4 e4]|[g4 a4] // Random choice between groups
```

## Euclidean Rhythms

Distribute k hits across n steps using the Euclidean algorithm.

| Syntax | Description |
|--------|-------------|
| `atom(k,n)` | k hits in n steps |
| `atom(k,n,r)` | With r-step rotation |

**Examples:**
```
bd(3,8)         // 3 hits in 8 steps: x..x..x.
bd(3,8,1)       // Same, rotated 1 step: ..x..x.x
sd(5,8)         // 5 hits in 8 steps: x.xx.xx.
hh(7,16)        // 7 hits in 16 steps
```

## Event Fields

When using patterns with pipe binding (`as`), event fields are accessible.

| Field | Aliases | Type | Description |
|-------|---------|------|-------------|
| `%.freq` | `%.f`, `%.pitch` | Hz | Frequency |
| `%.vel` | `%.v`, `%.velocity` | 0-1 | Velocity |
| `%.trig` | `%.t`, `%.trigger` | pulse | Trigger signal |
| `%.note` | `%.n`, `%.midi` | 0-127 | MIDI note number |
| `%.dur` | - | cycles | Duration |
| `%.chance` | - | 0-1 | Probability |
| `%.time` | - | cycles | Event time |
| `%.phase` | - | 0-1 | Phase within cycle |

**Example:**
```akkado
pat("c4 e4 g4") as e |> osc("sin", e.freq) |> % * e.vel |> out(%, %)
```

## Chord Qualities

### Triads

| Quality | Intervals | Example |
|---------|-----------|---------|
| (empty) | 0, 4, 7 | `C` (C major) |
| `m` | 0, 3, 7 | `Am` |
| `maj` | 0, 4, 7 | `Cmaj` |
| `min` | 0, 3, 7 | `Cmin` |
| `dim` | 0, 3, 6 | `Bdim` |
| `aug` | 0, 4, 8 | `Caug` |
| `sus2` | 0, 2, 7 | `Dsus2` |
| `sus4` | 0, 5, 7 | `Dsus4` |
| `5` | 0, 7 | `C5` (power chord) |

### Seventh Chords

| Quality | Intervals | Example |
|---------|-----------|---------|
| `7` | 0, 4, 7, 10 | `G7` (dominant) |
| `maj7` | 0, 4, 7, 11 | `Cmaj7` |
| `m7` | 0, 3, 7, 10 | `Am7` |
| `min7` | 0, 3, 7, 10 | `Amin7` |
| `dom7` | 0, 4, 7, 10 | `Cdom7` |
| `dim7` | 0, 3, 6, 9 | `Bdim7` |
| `m7b5` | 0, 3, 6, 10 | `Bm7b5` (half-dim) |
| `minmaj7` | 0, 3, 7, 11 | `Cminmaj7` |

### Extended Chords

| Quality | Intervals | Example |
|---------|-----------|---------|
| `9` | 0, 4, 7, 10, 14 | `G9` |
| `maj9` | 0, 4, 7, 11, 14 | `Cmaj9` |
| `min9` | 0, 3, 7, 10, 14 | `Am9` |
| `add9` | 0, 4, 7, 14 | `Cadd9` |
| `6` | 0, 4, 7, 9 | `C6` |
| `min6` | 0, 3, 7, 9 | `Am6` |

## Strudel/Tidal Compatibility

Akkado's mini-notation is designed to be compatible with Strudel and Tidal:

| Feature | Akkado | Strudel/Tidal |
|---------|--------|---------------|
| Rest symbol | `~` | `~` (same) |
| Elongation | `_` = extend previous note | `_` = extend previous note (same) |
| Sample variant | `bd:2` | `bd:2` (same) |
| Native chords | `Am` in pattern | `chord("Am")` function |
| Uppercase pitch | Requires octave (`A4`) | Often defaults to octave 4 |
| `.` shorthand | Not supported | `][` equivalent |

### Key Differences

1. **Chord notation**: Akkado parses `Am`, `C7`, `Fmaj7` directly in mini-notation. Strudel typically uses `chord("Am")` or similar functions.

2. **Case handling**: Uppercase letters without octaves (`A`, `C`, `G`) are parsed as chords in Akkado. Add an octave (`A4`) to force pitch interpretation.

## Examples

### Simple Melody
```
pat("c4 e4 g4 e4")
```

### With Rests
```
pat("c4 ~ e4 ~ g4 ~ e4 ~")
```

### Chord Progression
```
pat("Am F C G")
```

### Euclidean Drum Pattern
```
pat("bd(3,8) sd(2,8) hh(5,8)")
```

### Polyrhythm
```
pat("[c4 e4 g4, d4 f4]")
```

### Complex Pattern
```
pat("<c4 e4>*2 [g4 a4]?0.5 b4/2")
```

### With Alternation
```
pat("<[c4 e4] [d4 f4] [e4 g4]>")
```

---
title: Sequencing & Timing
category: builtins
order: 4
keywords: [sequencing, timing, lfo, trigger, euclid, euclidean, timeline, clock, rhythm, pattern, poly, mono, legato, spread, polyphony, polyphonic, voice, voices, chord, instrument]
---

# Sequencing & Timing

Timing and sequencing functions create rhythmic patterns, triggers, and automation curves synchronized to the global clock.

## clock

**Clock** - Returns the current clock position.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| -     | -    | -       | No parameters |

Returns the current position in the clock cycle. Use with other timing functions or for sync.

```akk
// Use clock for tempo-synced effects
osc("saw", 110) |> delay(%, clock() / 4, 0.4) |> out(%, %)
```

Related: [trigger](#trigger), [lfo](#lfo)

---

## lfo

**LFO** - Low Frequency Oscillator with optional duty cycle.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| rate  | signal | -       | Rate in Hz |
| duty  | number | 0.5     | Duty cycle for pulse (0-1) |

A low-frequency oscillator for modulation. The duty parameter controls the pulse width.

```akk
// Vibrato
osc("sin", 220 + lfo(5) * 10) |> out(%, %)
```

```akk
// Tremolo
osc("saw", 220) * (0.5 + lfo(4) * 0.5) |> out(%, %)
```

```akk
// Filter sweep
osc("saw", 110) |> lp(%, 500 + lfo(0.2) * 1500) |> out(%, %)
```

Related: [clock](#clock), [trigger](#trigger)

---

## trigger

**Trigger** - Generates trigger pulses at division of the beat.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| div   | number | -       | Triggers per beat |

Generates short impulses at regular intervals. A div of 4 means 4 triggers per beat (16th notes at 4/4).

```akk
// Kick drum on quarter notes
osc("sin", 55) * ar(trigger(1), 0.01, 0.2) |> out(%, %)
```

```akk
// Hi-hat on 8th notes
osc("noise") |> hp(%, 8000) * ar(trigger(2), 0.001, 0.05) |> out(%, %)
```

```akk
// Fast arpeggio triggers
pat("c4 e4 g4 c5") |> ((f) -> osc("saw", f) * ar(trigger(8))) |> out(%, %)
```

Related: [euclid](#euclid), [lfo](#lfo)

---

## euclid

**Euclidean Rhythm** - Generates Euclidean rhythm patterns.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| hits  | number | -       | Number of hits in pattern |
| steps | number | -       | Total steps in pattern |
| rot   | number | 0       | Rotation offset |

Creates rhythms by distributing hits as evenly as possible across steps. Classic patterns: (3,8) = Cuban tresillo, (5,8) = West African bell.

```akk
// Tresillo pattern
osc("sin", 55) * ar(euclid(3, 8), 0.01, 0.15) |> out(%, %)
```

```akk
// West African bell
osc("noise") |> hp(%, 6000) * ar(euclid(5, 8), 0.001, 0.03) |> out(%, %)
```

```akk
// Rotated pattern
osc("saw", 110) * ar(euclid(5, 16, 2)) |> lp(%, 800) |> out(%, %)
```

Related: [trigger](#trigger), [timeline](#timeline)

---

## timeline

**Timeline** - Breakpoint automation envelope.

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| -     | -    | -       | Configured via pattern syntax |

Creates smooth automation curves between breakpoints. Used for complex parameter automation synced to the clock.

```akk
// Volume automation
osc("saw", 220) * timeline() |> out(%, %)
```

Related: [lfo](#lfo), [trigger](#trigger)

---

## poly

**Polyphonic Voice Manager** - Allocates voices for a pattern and runs an instrument function per voice.

| Param      | Type     | Default | Description |
|------------|----------|---------|-------------|
| input      | pattern  | -       | Pattern or `chord(...)` producing events (notes or chords) |
| instrument | function | -       | A function `(freq, gate, vel) -> signal` run per voice |
| voices     | number   | 64      | Voice count (1-128, must be a literal) |

`poly()` reads pattern events at runtime and assigns each note to its own voice slot. The instrument function receives per-voice frequency, gate, and velocity, and the outputs of all active voices are summed. When the same note appears in consecutive events, the voice slot is reused (preserving phase continuity); when all voices are busy, the oldest is stolen.

The instrument must be a 3-parameter function — the names don't matter but the order is fixed: `(freq, gate, vel)`.

```akk
// Polyphonic chord progression with the default 64 voices
stab = (freq, gate, vel) ->
    saw(freq) * ar(gate, 0.05, 0.4) * vel
    |> lp(@, 1100)

chord("C Em Am G") |> poly(@, stab) |> out(@)
```

```akk
// Lower voice count if you want predictable stealing
pat("c4 e4 g4 b4") |> poly(@, (f, g, v) -> osc("sin", f) * v, 8) |> out(@)
```

```akk
// Inline closure
pat("c3 e3 g3") |> poly(@, (f, g, v) -> saw(f) * adsr(g, 0.01, 0.1, 0.7, 0.3)) |> out(@)
```

Related: [mono](#mono), [legato](#legato), [spread](#spread), [chord](pattern-mini-notation), [pat](pattern-mini-notation)

---

## mono

**Monophonic Voice Manager** - Single-voice manager with retrigger on every new note.

| Param      | Type     | Default | Description |
|------------|----------|---------|-------------|
| input      | pattern  | -       | Pattern producing events |
| instrument | function | -       | A function `(freq, gate, vel) -> signal` |

`mono()` is `poly()` with one voice and last-note priority. Every new note retriggers the gate, so envelopes restart cleanly — the classic hardware-mono behavior.

`mono(stereo_signal)` is a different builtin (stereo-to-mono downmix). The compiler routes based on argument type: a function instrument gets the voice manager; a stereo signal gets the downmix.

```akk
// Mono lead with retrigger on every note
pat("c4 e4 g4 c5") |> mono((f, g, v) -> saw(f) * adsr(g, 0.01, 0.1, 0.6, 0.3)) |> out(@)
```

Related: [poly](#poly), [legato](#legato)

---

## legato

**Legato Voice Manager** - Single-voice with no retrigger between connected notes.

| Param      | Type     | Default | Description |
|------------|----------|---------|-------------|
| input      | pattern  | -       | Pattern producing events |
| instrument | function | -       | A function `(freq, gate, vel) -> signal` |

Like `mono()`, but the gate stays high while notes overlap, so envelopes don't restart on every note. Frequency and velocity update but the AR/ADSR keeps decaying through the phrase. Best for legato leads and bass lines.

```akk
// Smooth bassline — gate stays high across notes
pat("c2 e2 g2 c3") |> legato((f, g, v) -> saw(f) * adsr(g, 0.01, 0.2, 0.8, 0.4)) |> out(@)
```

Related: [poly](#poly), [mono](#mono)

---

## spread

**Spread** - Distribute an array across N voices evenly.

| Param  | Type   | Default | Description |
|--------|--------|---------|-------------|
| n      | number | -       | Number of voices to spread across |
| source | array  | -       | Source array of values |

`spread()` is a compile-time helper that takes an array and distributes its values across `n` slots, repeating or reducing as needed. Useful for unison voices and detuned stacks.

```akk
// Detuned saw stack — 5 oscillators across the array
osc("saw", spread(5, [220, 220.7, 219.3, 221.4, 218.6])) |> out(@)
```

Related: [poly](#poly)

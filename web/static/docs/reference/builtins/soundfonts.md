---
title: SoundFonts
category: builtins
order: 16
keywords: [soundfont, sf2, sf3, gm, general-midi, preset, voice, polyphonic, instrument, piano, fluidsynth, timgm6mb, fluidr3, musescore]
---

# SoundFonts

SoundFonts (.sf2 / .sf3) are bundled instrument banks — typically General MIDI piano, strings, brass, drums. The `soundfont()` builtin plays a pattern through a chosen preset of a chosen SoundFont file. nkido ships with three GM banks: `gm` (small, preloaded), `gm_medium` (FluidR3Mono), and `gm_large` (MuseScore General).

## soundfont

**SoundFont playback** - Polyphonic playback of a pattern through a SoundFont preset.

| Param   | Type    | Default | Description |
|---------|---------|---------|-------------|
| pattern | pattern | -       | Pattern producing gate / freq / velocity events |
| file    | string  | -       | SoundFont filename (string literal) |
| preset  | number  | -       | Preset index within the SoundFont (number literal) |

The pattern provides gate, frequency, and velocity signals. The file is resolved at compile time. The preset is the GM program number — `0` is Acoustic Grand Piano, `40` is Violin, etc.

```akk
// Piano on a chord progression
chord("C Em Am G") |> soundfont(@, "gm", 0) |> out(@)
```

## gm

The **General MIDI** bank, preloaded on engine startup. Filename `"gm"` resolves to `TimGM6mb.sf3` — a small, fast-loading set of GM presets covering the standard 128 instruments + drum kits.

```akk
// Acoustic Grand Piano (preset 0)
pat("c4 e4 g4 c5") |> soundfont(@, "gm", 0) |> out(@)
```

## preset

The **preset index** picks an instrument within the SoundFont. For GM banks: `0` Acoustic Grand Piano, `4` Electric Piano, `24` Acoustic Guitar, `40` Violin, `56` Trumpet, `73` Flute, `81` Lead Synth, `128` (channel 10) Drum Kit. See the General MIDI specification for the full table.

```akk
// Strings ensemble (preset 48)
chord("Am F C G") |> soundfont(@, "gm", 48) |> out(@)
```

## voice

`soundfont()` is **polyphonic** — multiple notes from a chord pattern play simultaneously, each on its own voice. Voice allocation is automatic; use a `chord(...)` or `poly(pat, ...)` input pattern to get parallel notes.

```akk
// Polyphonic chord voicing
chord("Cmaj7 Fmaj7 G7 Cmaj7") |> soundfont(@, "gm", 0) |> out(@)
```

## polyphonic

Voice count is managed internally by the SoundFont engine. There's no explicit voice limit at the akkado level — the engine steals the oldest voice when the budget is exhausted, which is typical SoundFont behavior.

## instrument

Different presets within a SoundFont give different **instruments**. The same pattern routed through different `preset` values produces, e.g., a piano version, a brass version, and a string version of the same melody — useful for layering.

```akk
// Layered piano + strings
melody = chord("Am F C G")
melody |> soundfont(@, "gm", 0) +
melody |> soundfont(@, "gm", 48) |> out(@)
```

Related: [samplers](samplers), [samples-loading](samples-loading), [poly](sequencing#poly)

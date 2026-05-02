---
title: Samplers
category: builtins
order: 15
keywords: [sampler, sample, sample_loop, drum, kit, "808", "909", bd, sd, hh, oh, cp, percussion, playback, one-shot, loop]
---

# Samplers

Samplers play recorded audio. The common path is a **sample pattern** like `pat("bd sd hh sd")`, which triggers events from the loaded sample bank. nkido ships with a default 808 drum kit so common drum names work out of the box. For more control, `sample()` and `sample_loop()` play a single sample by ID.

## sample

**One-shot sampler** - Trigger a sample by ID.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| trig  | signal | -       | Trigger signal (rising edge plays sample) |
| id    | number | -       | Sample slot index in the loaded bank |

Plays the sample once on every rising-edge trigger. The sample stops at its natural end.

```akk
// Trigger sample 0 on each beat
sample(trigger(1), 0) |> out(%, %)
```

## sample_loop

**Looping sampler** - Trigger a sample with looping playback.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| trig  | signal | -       | Trigger signal |
| id    | number | -       | Sample slot index |

Plays the sample looped. Useful for sustained material like sustained chords or pads recorded as samples.

```akk
// Looped pad sample
sample_loop(trigger(0.25), 5) |> out(%, %)
```

## bd

The **bass drum**. Default kit slot for `bd` patterns; variants `bd2` through `bd16` cover different kicks from the 808 bank.

```akk
// Four-on-the-floor kick
pat("bd bd bd bd") |> out(%, %)
```

## sd

The **snare drum**. Default kit slot for `sd`; variants `sd2`–`sd8` give alternate snares.

```akk
// Backbeat
pat("~ sd ~ sd") |> out(%, %)
```

## hh

The **hi-hat closed**. Variants like `oh` (open hat), `cp` (clap), `rim` (rimshot) round out the percussion set.

```akk
// 16th-note hats
pat("hh*16") |> out(%, %)
```

## oh

The **open hi-hat**. Use it to vary the closed-hat pattern; typical syncopation drops an `oh` on the off-beat.

```akk
// Closed/open hat variation
pat("hh hh oh hh") |> out(%, %)
```

## cp

The **clap**. Layered on the backbeat for a snare reinforcement.

```akk
// Snare + clap layered
pat("~ [sd cp] ~ [sd cp]") |> out(%, %)
```

Related: [samples-loading](samples-loading), [soundfonts](soundfonts), [sequencing](sequencing)

---
title: Visualizations
category: builtins
order: 12
keywords: [visualization, viz, oscilloscope, waveform, spectrum, waterfall, pianoroll, piano, roll, display, monitor, probe, fft, frequency, spectrogram, trigger, logScale, minDb, maxDb, filled, showGrid, gradient]
---

# Visualizations

Inline visualizations that render directly in the editor. Each visualization is inserted as a pass-through node in the signal chain — the audio passes through unchanged while being displayed.

All visualization functions accept an optional name string and an options record. Common options shared by all types:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| width  | number / string | 200 | Width in pixels, or `"100%"` for full width |
| height | number / string | 50 | Height in pixels, or `"100%"` for full height |

## oscilloscope

**Oscilloscope** - Time-domain waveform display with trigger stabilization.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option       | Type   | Default    | Description |
|--------------|--------|------------|-------------|
| triggerLevel | number | 0          | Amplitude threshold for trigger |
| triggerEdge  | string | `"rising"` | Trigger edge: `"rising"` or `"falling"` |

Displays a real-time oscilloscope view of the signal. The trigger stabilizes the waveform so it doesn't drift horizontally. Set `triggerLevel` to the amplitude where the waveform should "lock" and `triggerEdge` to choose which direction of crossing to trigger on.

```akk
// Basic oscilloscope
osc("sin", 440) |> oscilloscope(%) |> out(%, %)
```

```akk
// Named with trigger at zero-crossing
osc("saw", 110) |> oscilloscope(%, "saw wave", {triggerLevel: 0, triggerEdge: "rising"}) |> out(%, %)
```

```akk
// Wide display with falling edge trigger
osc("sqr", 220) |> oscilloscope(%, "square", {width: 400, triggerEdge: "falling"}) |> out(%, %)
```

Related: [waveform](#waveform), [spectrum](#spectrum)

---

## waveform

**Waveform** - Time-domain envelope display showing min/max amplitude.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option | Type    | Default | Description |
|--------|---------|---------|-------------|
| scale  | number  | 1.0     | Amplitude multiplier (2.0 = zoom in 2x) |
| filled | boolean | true    | Filled envelope or line waveform |

Displays the signal as a min/max envelope over time. In filled mode (default), shows a shaded region between the minimum and maximum amplitude at each point. In line mode (`filled: false`), draws a single center-line waveform similar to the oscilloscope.

```akk
// Basic waveform
osc("saw", 110) |> waveform(%) |> out(%, %)
```

```akk
// Zoomed in with line style
osc("sin", 440) * 0.3 |> waveform(%, "quiet signal", {scale: 3.0, filled: false}) |> out(%, %)
```

```akk
// Wide filled envelope
osc("saw", 55) |> lp(%, 400) |> waveform(%, "bass", {width: 400, height: 80}) |> out(%, %)
```

Related: [oscilloscope](#oscilloscope), [spectrum](#spectrum)

---

## spectrum

**Spectrum Analyzer** - Frequency-domain FFT display.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option   | Type    | Default | Description |
|----------|---------|---------|-------------|
| fft      | number  | 1024    | FFT size: 256, 512, 1024, or 2048 |
| logScale | boolean | false   | Logarithmic frequency axis |
| minDb    | number  | -90     | Minimum dB for display range |
| maxDb    | number  | 0       | Maximum dB for display range |

Displays a real-time frequency spectrum using FFT analysis. Linear scale (default) spaces frequencies evenly; logarithmic scale (`logScale: true`) gives more detail to low frequencies, matching how we perceive pitch. Larger FFT sizes give better frequency resolution but slower updates.

```akk
// Basic spectrum
osc("saw", 220) |> spectrum(%) |> out(%, %)
```

```akk
// Log scale with custom dB range
osc("saw", 110) |> lp(%, 2000) |> spectrum(%, "filtered", {logScale: true, minDb: -60, maxDb: 0}) |> out(%, %)
```

```akk
// High resolution FFT
osc("saw", 55) |> spectrum(%, "bass detail", {fft: 2048, logScale: true, width: 400}) |> out(%, %)
```

Related: [waterfall](#waterfall), [oscilloscope](#oscilloscope)

---

## waterfall

**Waterfall / Spectrogram** - Time-frequency display showing spectrum evolution over time.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Input signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option   | Type   | Default | Description |
|----------|--------|---------|-------------|
| fft      | number | 1024    | FFT size: 256, 512, 1024, or 2048 |
| gradient | string | `"magma"` | Color gradient preset |
| angle    | number | 180     | Scroll direction in degrees |
| speed    | number | 40      | Scroll speed in pixels per second |
| minDb    | number | -90     | Minimum dB for color mapping |
| maxDb    | number | 0       | Maximum dB for color mapping |

Displays a scrolling spectrogram where the x-axis is time, the y-axis is frequency, and color represents amplitude. Available gradient presets: `"magma"`, `"viridis"`, `"inferno"`, `"thermal"`, `"grayscale"`.

The default size for waterfall is 300x150 (larger than other visualizations).

```akk
// Basic waterfall
osc("saw", 220) |> waterfall(%) |> out(%, %)
```

```akk
// Viridis gradient with high-res FFT
osc("saw", 110) |> lp(%, 1000 + osc("sin", 0.5) * 2000) |> waterfall(%, "sweep", {gradient: "viridis", fft: 2048}) |> out(%, %)
```

```akk
// Custom size and dB range
osc("saw", 55) |> waterfall(%, "bass", {width: 500, height: 200, minDb: -60, maxDb: 0, gradient: "thermal"}) |> out(%, %)
```

Related: [spectrum](#spectrum)

---

## pianoroll

**Piano Roll** - Pattern event visualization on a pitch-time grid.

| Param | Type   | Description |
|-------|--------|-------------|
| in    | signal | Pattern signal |
| name  | string | Display label (optional) |
| opts  | record | Options (optional) |

**Options:**

| Option   | Type    | Default      | Description |
|----------|---------|--------------|-------------|
| beats    | number  | auto         | Number of beats visible in the window |
| showGrid | boolean | true         | Show beat grid lines |
| scale    | string  | `"chromatic"` | Scale filter: `"chromatic"`, `"pentatonic"`, or `"octave"` |

Displays pattern events as rectangles on a piano roll grid. The playhead scrolls through the pattern in real time. The `scale` option dims notes that fall outside the selected scale, making it easy to see which notes are in-key. `"pentatonic"` highlights C major pentatonic (C, D, E, G, A); `"octave"` highlights only root notes (C).

```akk
// Basic piano roll
pat("c4 e4 g4 b4") |> pianoroll(%)
```

```akk
// Wide view with 8 beats visible
pat("c4 e4 g4 c5 ~ e4 g4 b4") |> pianoroll(%, "melody", {beats: 8, width: 400, height: 80})
```

```akk
// Pentatonic scale filter, no grid
pat("c4 d4 e4 f4 g4 a4 b4 c5") |> pianoroll(%, "scale check", {scale: "pentatonic", showGrid: false})
```

Related: [oscilloscope](#oscilloscope), [spectrum](#spectrum)

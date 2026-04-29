> **Status: NOT STARTED** — No wavetable system exists.

# Product Requirement Document: "Smooch" Wavetable Oscillator

**Project:** Cedar (live-coding audio engine) + Akkado language

**Codename:** Smooch — affectionate name for the project.

**Cedar opcode:** `OSC_WAVETABLE`

**Akkado builtins:** `wt`, `wave`, `wavetable`, `smooch` (all aliases for the same opcode)

---

## 1. Executive Summary

Add a production-grade wavetable oscillator to Cedar comparable to Serum or Phase Plant. Live coders load a wavetable file once with `wt_load(...)`, then any number of `smooch(freq, phase, tablePos)` voices read from the active bank. The system is split into a heavy offline preprocessor (FFT-based mip-map generation) and a lock-free per-sample audio-thread reader, so loading never blocks playback.

**Why now?**
- Cedar already ships analytic oscillators (sin/saw/tri/square with PolyBLEP/MinBLEP) and SoundFont voices, but has no wavetable synthesis. Wavetables are a distinct timbral palette and a category-defining feature for the engine's positioning against Serum / Phase Plant / Vital.
- All required dependencies are already in place: `kiss_fftr` is vendored at `cedar/third_party/kissfft/`, the file-loading abstraction landed in commit `a3283d3`, and the polyphony system (`POLY` opcode) is shipped.

---

## 2. Goals and Non-Goals

### 2.1 Goals

- **Zero aliasing** across the audible range, achieved via per-octave band-limited mip-maps with smooth crossfading at boundaries.
- **Low noise floor** through 4-point Hermite interpolation in the audio loop.
- **Live-coding stability**: loading and FFT preprocessing must run off the audio thread; the audio path must be lock-free and allocation-free.
- **Standard file compatibility**: load Serum-style 2048-sample WAV frames with no custom format.
- **Polyphonic by default**: any number of `smooch` voices share one immutable in-memory bank.
- **Hot-swap safe**: a code reload that keeps the same `OSC_WAVETABLE` node preserves phase state via Cedar's existing semantic-ID matching.

### 2.2 Non-Goals

- **No custom `.wt` file format** (Serum's, Vital's, or any vendor-specific). We load plain WAV with a frame-size convention.
- **No multiple simultaneous banks in v1**: only one wavetable bank is active at a time. Multi-bank support is a v2 follow-up.
- **No on-the-fly bank editing** (drawing, additive editing, audio-import). Banks are immutable resources.
- **No spectral morphing** (interpolating in the frequency domain between frames). v1 uses time-domain linear blend between adjacent frames; spectral morph is a v2 follow-up.
- **No FM/PM/sync** between smooch voices. Standard FM via frequency-rate input modulation is supported (since `freq` is an audio-rate buffer); deeper FM features are out of scope.

---

## 3. Technical Architecture

The system has three layers:

```
[ Akkado source ]
       |
       v
[ wt_load("name", "path") ]   — Registers a bank in the global WavetableBankRegistry
       |
       v
[ WavetableBank ]              — Immutable: heap-allocated, FFT-prepared, shared across voices
       |
       v
[ OSC_WAVETABLE opcode ]       — Per-voice state in StatePool, lock-free per-sample read
```

### 3.1 WavetableBankRegistry (singleton, host-thread only)

- Owns the loaded `WavetableBank` (only one active bank in v1).
- Replaced wholesale on each `wt_load(...)` call.
- The active bank is referenced from the audio thread via an atomic shared-pointer swap at block boundaries (same triple-buffer pattern Cedar already uses for compiled programs).

### 3.2 WavetableBank (resource / heap)

- Stores the actual sample data: an array of `WavetableFrame`s.
- Heavy initialization (FFT, mip generation) happens at load time.
- Immutable during audio playback.
- Shared by reference across all `OSC_WAVETABLE` voices.

### 3.3 OSC_WAVETABLE opcode (DSP / per-voice)

- Lightweight per-voice state held in `StatePool` (phase accumulator + cached bank pointer).
- Performs phase advance, mip selection with crossfade, frame morph, and Hermite interpolation per sample.
- Reads exclusively from the immutable `WavetableBank`; writes only to its own `SmoochState`.

---

## 4. Data Structures

### 4.1 The Wavetable Pyramid (Mip-Maps)

To prevent aliasing, every waveform frame is pre-calculated into a series of band-limited versions (one per octave).

- **Base table size:** 2048 samples (Serum/Massive convention).
- **Mip-map levels:** `MAX_MIP_LEVELS = 11`, indexed 0..10.
  - Table 0: full spectrum (1024 harmonics — N/2).
  - Table 1: 512 harmonics.
  - Table 2: 256 harmonics.
  - …
  - Table 10: 1 harmonic (fundamental only).

```cpp
constexpr int WAVETABLE_SIZE = 2048;
constexpr int MAX_MIP_LEVELS = 11;

// One shape (e.g., a single waveform from "Basic Shapes.wav")
struct WavetableFrame {
    // Always size MAX_MIP_LEVELS x WAVETABLE_SIZE.
    // Each inner array holds the IFFT'd, peak-normalized, band-limited waveform
    // at the corresponding octave. Always 2048 samples to keep the read loop
    // branch-free on mask wrapping.
    std::array<std::array<float, WAVETABLE_SIZE>, MAX_MIP_LEVELS> mipMaps;
};

// One loaded bank (e.g., "Basic Shapes.wav")
struct WavetableBank {
    std::vector<WavetableFrame> frames;       // Morphable frames, in order.
    int tableSize    = WAVETABLE_SIZE;        // Cached for hot-loop reads.
    int numMipLevels = MAX_MIP_LEVELS;
};
```

### 4.2 Per-voice state

```cpp
struct SmoochState {
    double phase           = 0.0;             // Phase accumulator [0, 1)
    bool   initialized     = false;
    // No bank pointer here: the bank is fetched from the registry once per block
    // and cached on the ExecutionContext.
};
```

### 4.3 File loading: Serum 2048-frame WAV convention

We do **not** define a custom `.wt` format. Instead we adopt the Serum/Surge XT convention:

- The wavetable is a plain mono WAV file whose total sample count is a multiple of 2048.
- Each consecutive 2048-sample chunk is one frame.
- Sample format: 16-bit PCM, 24-bit PCM, or 32-bit float (any rate; rate is irrelevant — these are *one cycle of waveform per frame*, not time-domain audio).
- Loading uses Cedar's existing `cedar::WavLoader::load_from_file()` (see `prd-file-loading-abstraction.md`, status DONE in `a3283d3`).
- If the file's sample count is not a multiple of 2048, loading fails with a clear error.

This is compatible with wavetables exported from Serum, Surge XT, Vital (after WAV export), and Bitwig.

### 4.4 Memory budget

| Item                       | Size                                             |
| -------------------------- | ------------------------------------------------ |
| One mip level              | 2048 samples × 4 B = 8 KB                        |
| One frame (11 mip levels)  | 8 KB × 11 = 88 KB                                |
| 64-frame bank (Serum size) | 88 KB × 64 = **5.6 MB**                          |
| 256-frame bank             | 88 KB × 256 = **22.5 MB**                        |
| Per-voice state            | 16 B (one double + bool + padding)               |

Cedar's `MAX_ARENA_SIZE = 128 MB` for audio buffers; wavetable banks live in their own host-side allocation outside the arena and are pointed to from `ExecutionContext`. Recommended max bank size: **32 MB** (~360 frames at 88 KB each), enforced as a soft limit at load time. WASM heap planning should reserve at least 64 MB above baseline for wavetable banks.

### 4.5 Alternative mip-pyramid spacings (follow-up exploration)

Octave spacing with 11 levels is the standard wavetable layout (Serum, Surge XT, Vital all use it) and is the right v1 default — predictable memory budget, simple `log2` mip selection, well-understood crossfade behavior. But spacing is a tradeoff knob between memory, crossfade-region width, and the audibility of spectral steps during pitch sweeps. Alternatives worth exploring once the v1 baseline is in place:

- **Half-octave spacing (~22 levels).** Halves the spectral gap between adjacent mips, so the crossfade region in §6.3 step 2 carries less perceptual load. Doubles bank size (~11 MB for a 64-frame bank instead of 5.6 MB) — still well under the 32 MB soft cap.
- **Logarithmic with finer resolution at the top.** Use octave spacing in the lower mips (where the ear is less sensitive to small bandwidth differences) and quarter-octave at the top (where alias artifacts are most exposed). Modest memory increase, biggest perceptual win.
- **Adaptive / content-driven spacing.** Choose mip cutoffs per-frame based on where the source's harmonic energy lives. A frame with content concentrated below the 8th harmonic doesn't need a high-resolution top mip.
- **Single-table with on-the-fly oversampling.** Skip the pyramid entirely; oversample 2× or 4× at runtime and lowpass-filter. Simpler memory model and zero crossfade artifacts, but moves cost from preprocessor to audio thread — likely a non-starter for polyphonic use.
- **Fewer levels with wider per-level bandwidth.** 6–7 levels at 1.5–2 octave spacing, leaning more heavily on the runtime crossfade. Smaller banks at the cost of more aggressive crossfade work.

This is **deliberately deferred**. v1 ships with octave spacing × 11 levels. The §12 tests (#2 aliasing sweep, #4 mip-boundary crossfade) measure how aggressive the crossfade has to work; if those tests show audible artifacts at boundaries, half-octave or top-weighted spacing is the obvious next step.

---

## 5. Algorithm 1: Pre-Processing (offline, host thread)

Runs once at `wt_load(...)`. Converts each loaded frame into the 11-level mip pyramid.

**Dependencies:** `kiss_fftr` (already vendored at `cedar/third_party/kissfft/`).

**Note on existing FFT API:** Cedar's `cedar/dsp/fft.hpp` exposes `compute_fft()` / `compute_ifft()` but applies a Hanning window inside `compute_fft()`. Windowing is correct for spectrum *visualization* but wrong for wavetable *generation* — we need the raw spectrum so the IFFT round-trips losslessly. Therefore the wavetable preprocessor calls `kiss_fftr` and `kiss_fftri` directly (or we add a non-windowed variant to `fft.hpp`).

### 5.1 Steps (per source frame)

1. **Load source waveform.** 2048 floats from the WAV chunk.
2. **Forward FFT.** Get full complex spectrum (1025 bins).
3. **For each octave `k = 0..10`:**
   1. Copy the source spectrum.
   2. Compute `cutoffBin = 1024 >> k` (Table 0: 1024 bins kept; Table 10: 1 bin).
   3. **Spectral filter with raised-cosine taper.** Inside the passband (`bin < cutoffBin - taperWidth`), keep bins unchanged. Across the taper region (last `taperWidth = 4` bins), apply a half raised-cosine ramp from 1.0 → 0.0. Above `cutoffBin`, zero out. The taper suppresses Gibbs ringing that a hard brickwall would introduce in the time domain.
   4. **Inverse FFT** back to 2048 time-domain samples.
   5. **Scale** by `1.0f / N` (kissfft does not normalize internally).
   6. **Peak-normalize** the resulting waveform so its peak amplitude matches the source frame's peak amplitude. Without this, lower-bandwidth mips lose energy and the oscillator audibly drops in level as pitch sweeps upward through octave boundaries.
   7. Store into `frame.mipMaps[k]`.

### 5.2 C++ pseudo-implementation

```cpp
#include "kissfft/kiss_fftr.h"

void generateMipMaps(WavetableFrame& frame,
                     const std::array<float, WAVETABLE_SIZE>& sourceData) {
    constexpr int N = WAVETABLE_SIZE;        // 2048
    constexpr int taperWidth = 4;
    kiss_fftr_cfg fwd = kiss_fftr_alloc(N, 0, nullptr, nullptr);
    kiss_fftr_cfg inv = kiss_fftr_alloc(N, 1, nullptr, nullptr);

    std::vector<kiss_fft_cpx> spectrum(N / 2 + 1);
    kiss_fftr(fwd, sourceData.data(), spectrum.data());

    // Cache source peak for amplitude normalization
    float srcPeak = 0.0f;
    for (float s : sourceData) srcPeak = std::max(srcPeak, std::abs(s));
    if (srcPeak < 1e-9f) srcPeak = 1.0f;

    for (int k = 0; k < MAX_MIP_LEVELS; ++k) {
        // Copy spectrum
        std::vector<kiss_fft_cpx> filtered = spectrum;

        // Cutoff and taper
        int cutoffBin = (N / 2) >> k;            // 1024, 512, 256, ..., 1
        int taperStart = std::max(0, cutoffBin - taperWidth);
        for (int bin = taperStart; bin < (N / 2 + 1); ++bin) {
            float gain;
            if (bin >= cutoffBin) {
                gain = 0.0f;
            } else {
                // Half raised-cosine: 1.0 at taperStart → 0.0 at cutoffBin
                float t = float(bin - taperStart) / float(taperWidth);
                gain = 0.5f * (1.0f + std::cos(t * 3.14159265358979f));
            }
            filtered[bin].r *= gain;
            filtered[bin].i *= gain;
        }

        // Inverse FFT
        std::array<float, N> output{};
        kiss_fftri(inv, filtered.data(), output.data());

        // Scale and peak-normalize to match source amplitude
        for (auto& s : output) s /= float(N);
        float mipPeak = 0.0f;
        for (float s : output) mipPeak = std::max(mipPeak, std::abs(s));
        if (mipPeak > 1e-9f) {
            float gain = srcPeak / mipPeak;
            for (auto& s : output) s *= gain;
        }

        frame.mipMaps[k] = output;
    }

    kiss_fftr_free(fwd);
    kiss_fftr_free(inv);
}
```

### 5.3 Alternative band-limiting techniques (follow-up exploration)

Raised-cosine over 4 bins is a reasonable v1 default — well-understood, cheap, and a clear improvement over a hard brickwall — but it is unlikely to be the *best*-sounding option. Because the preprocessor runs offline, we have plenty of compute budget to experiment with more sophisticated approaches in a follow-up DSP-quality pass. Listening tests should drive the final choice, since psychoacoustic perception of the cutoff region (especially at the top of each mip's bandwidth) doesn't always match what the magnitude spectrum suggests. Candidates worth A/B'ing:

- **Wider raised-cosine** (e.g., 8 or 16 bins). Same shape, gentler slope. Less ringing at the cost of slightly more spectral overlap into the stop band — usually inaudible since the rolled-off harmonics are already at the top of the audible range.
- **Gaussian taper.** Smoothest possible rolloff (no derivative discontinuities at any order). Often perceived as "warmer" or "more natural" than cosine-based tapers. Tunable via standard deviation.
- **Kaiser window** applied to the spectrum. Tunable β parameter trades sidelobe rejection against transition-band width; lets us tune the taper without changing its shape family.
- **Tukey window** (flat top + cosine flanks). Generalizes our raised-cosine; with `α = 1` it *is* a half-cosine, with smaller α it's flatter in the passband.
- **Frequency-dependent taper width.** Narrower taper at high mip levels (where few bins remain to spend) and wider at low mip levels (where there's headroom). Could match perceptual bandwidth better than a uniform 4-bin taper.
- **Lanczos / windowed-sinc reconstruction.** Closer to the ideal lowpass without Gibbs ringing. Slightly more compute but still trivial offline.
- **Additive resynthesis from explicit harmonics.** Skip the spectral-filter approach entirely: extract harmonic amplitudes and phases at the source bins, then reconstruct each mip via additive synthesis using only the harmonics that fit. Maximally accurate for harmonic content but loses any inharmonic/noise content present in the source — a tradeoff that may or may not be desirable depending on the wavetable.
- **Iterative / least-squares FIR design** (Parks-McClellan or similar) for an "ideal" passband-stopband shape, applied as a multiplicative spectral mask. Overkill for v1 but useful as a reference against which simpler tapers can be compared.

This is **deliberately deferred**. v1 ships with raised-cosine; the test in §12 (#2 aliasing sweep, #4 mip-boundary crossfade) gives us an objective baseline, and subjective listening tests on a curated wavetable set will guide the choice of which alternatives to integrate.

### 5.4 Alternative amplitude-preservation strategies (follow-up exploration)

Peak-matching to the source frame (§5.1 step 6) is a reasonable v1 default — it prevents the obvious failure mode of audibly dropping level as pitch sweeps upward through octave boundaries — but it isn't the only normalization choice, and it can over-boost mips whose surviving harmonics happen to align constructively. Because normalization runs offline alongside the FFT, we have headroom to experiment. Candidates worth A/B'ing once a baseline test corpus exists:

- **RMS normalization.** Match each mip's RMS to the source RMS. Preserves *perceived loudness* better than peak, since the ear integrates over time. Risk: peaks in heavily-band-limited mips can clip if the source had a low crest factor and the filtered signal develops new peaks.
- **Loudness-weighted (LUFS / ITU-R BS.1770).** Apply a K-weighting filter before measuring level. Closest to "matches the way humans hear loudness," but adds a perceptual filter to every mip generation — overkill for v1.
- **Energy preservation (Parseval).** Scale each mip so the sum of squared spectral magnitudes equals the source's, before IFFT. Theoretically elegant; in practice often equivalent to RMS with extra steps.
- **Per-harmonic compensation.** Instead of normalizing the whole mip, individually scale surviving harmonics so each one's amplitude matches the source. Avoids the constructive-alignment over-boost of peak normalization. Requires interleaving normalization with the spectral filter from §5.1.3.
- **No normalization (preserve source dynamics).** Skip the rescale; accept the gradual level drop as harmonics roll off. Simplest; some users may actually prefer it as it preserves the source's spectral character verbatim.
- **Bank-wide normalization.** Normalize all mips to a single reference (e.g., the brightest frame's peak) rather than per-frame. Avoids morph-induced level jumps when adjacent frames have very different harmonic content.
- **Pre-FFT source conditioning.** Detect and remove DC offset, optionally high-pass at the fundamental, and align the fundamental phase across frames before FFT. Reduces the work the spectral filter has to do and improves frame-morph behavior (see §6.5.3).

This is **deliberately deferred**. v1 ships with peak-normalization. The §12 tests (#1 sine round-trip, #4 mip-boundary crossfade, #5 frame morph continuity) give us baselines for level matching; subjective listening on a curated bank will guide which alternative — most likely RMS or per-harmonic — replaces peak.

---

## 6. Algorithm 2: Runtime Opcode (`OSC_WAVETABLE`)

This runs on the audio thread inside Cedar's VM. It must be lock-free and allocation-free.

### 6.1 Cedar VM dispatch signature

Cedar opcodes are dispatched by the VM as `(ExecutionContext& ctx, const Instruction& inst)`. Inputs are 16-bit buffer IDs in `inst.inputs[0..4]`, read with `ctx.buffers->get(inst.inputs[i])`. Per-voice state is fetched once per call:

```cpp
auto& state = ctx.states->get_or_create<SmoochState>(inst.state_id);
const WavetableBank* bank = ctx.wavetable_bank;  // Cached on ctx by VM at block start
```

### 6.2 Inputs (audio-rate buffers)

| Slot          | Meaning                          |
| ------------- | -------------------------------- |
| `inputs[0]`   | `freq` — Hz                      |
| `inputs[1]`   | `phaseOffset` — [0, 1)           |
| `inputs[2]`   | `tablePos` — [0, frames.size())  |
| `inputs[3]`   | unused (reserved for trigger)    |
| `inputs[4]`   | unused (reserved for sync)       |

All three primary inputs are full audio-rate buffers; scalars are passed by Cedar's standard constant-buffer mechanism.

### 6.3 Per-sample execution flow

1. **Phase accumulation** (use `double` for precision):
   ```
   inc = freq[i] / sampleRate
   state.phase += inc
   while (state.phase >= 1.0) state.phase -= 1.0
   while (state.phase < 0.0)  state.phase += 1.0
   finalPhase = state.phase + phaseOffset[i]
   finalPhase -= floor(finalPhase)
   ```
2. **Fractional mip-level selection (with crossfading):**
   ```
   maxHarmonic   = (sampleRate * 0.5) / max(freq[i], 1e-3)
   mipFractional = clamp(log2(1024.0 / maxHarmonic), 0.0, 10.0)
   mipLow        = floor(mipFractional)
   mipFrac       = mipFractional - mipLow
   mipHigh       = min(mipLow + 1, MAX_MIP_LEVELS - 1)
   ```
   Note the corrected formula: `log2(1024 / maxHarmonic)`, where `1024 = N/2` is Table 0's harmonic count. (The original PRD's `log2(2048 / maxHarmonic)` was off by one octave.) The fractional part lets us crossfade between adjacent mips, eliminating audible spectral steps when frequency sweeps across an octave boundary.
3. **Frame selection** (clamp at end, no wrap):
   ```
   frameA   = floor(tablePos[i])
   morphFrac = tablePos[i] - frameA
   frameB   = min(frameA + 1, bank.frames.size() - 1)
   frameA   = clamp(frameA, 0, bank.frames.size() - 1)
   ```
4. **4-point Hermite interpolation** at both `mipLow` and `mipHigh`, both `frameA` and `frameB` — four reads per sample. All four index reads use `& mask` for safe wrapping:
   ```cpp
   constexpr int MASK = WAVETABLE_SIZE - 1;  // 2047
   float readPos = float(finalPhase) * WAVETABLE_SIZE;
   int   intPos  = int(readPos);
   float fracPos = readPos - intPos;
   ```
   For each `(frameIdx, mipIdx)` pair:
   ```cpp
   const auto& tbl = bank.frames[frameIdx].mipMaps[mipIdx];
   float y0 = tbl[(intPos - 1) & MASK];
   float y1 = tbl[intPos       & MASK];   // <-- masked, fixed from the original
   float y2 = tbl[(intPos + 1) & MASK];
   float y3 = tbl[(intPos + 2) & MASK];
   float s  = hermite(fracPos, y0, y1, y2, y3);
   ```
5. **Blend** (frame morph then mip crossfade):
   ```
   sigA_low  = hermite(frameA, mipLow)
   sigB_low  = hermite(frameB, mipLow)
   low       = sigA_low + (sigB_low - sigA_low) * morphFrac
   sigA_high = hermite(frameA, mipHigh)
   sigB_high = hermite(frameB, mipHigh)
   high      = sigA_high + (sigB_high - sigA_high) * morphFrac
   output[i] = low + (high - low) * mipFrac
   ```
6. **Hermite kernel** (unchanged from the original PRD):
   ```cpp
   inline float hermite(float x, float y0, float y1, float y2, float y3) {
       float c0 = y1;
       float c1 = 0.5f * (y2 - y0);
       float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
       float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
       return ((c3 * x + c2) * x + c1) * x + c0;
   }
   ```

### 6.4 State struct (rename from clashing `OscState`)

The original PRD reused the name `OscState`, but Cedar already has `OscState` in `cedar/include/cedar/opcodes/dsp_state.hpp`, used by every existing oscillator (`OSC_SIN`, `OSC_SAW`, etc.). The wavetable opcode uses a distinct `SmoochState` (defined in §4.2).

### 6.5 Alternative runtime techniques (follow-up exploration)

The audio-thread reader has three independent quality knobs that v1 fixes to reasonable defaults: the per-sample interpolation kernel, the mip-boundary crossfade curve, and the inter-frame morph blend. Each is a candidate for a follow-up DSP-quality pass once §12 baselines are in place. Unlike the offline alternatives in §5.3/§5.4, runtime alternatives have a CPU cost — listening tests must weigh quality against polyphony budget.

#### 6.5.1 Interpolation kernel

4-point Hermite (§6.3 step 4) is a clear step up from the linear interpolation used by cedar's existing samplers (`cedar/include/cedar/vm/sample_bank.hpp`) and is the standard wavetable choice. Alternatives, roughly in order of cost:

- **Linear (2-point).** Cheaper but visibly worse: drops ~20 dB of high-frequency response by the top of each mip's bandwidth. Useful only as a quality fallback for extreme polyphony.
- **4-point Lagrange.** Slightly different coefficient set than Hermite. Marginal sonic difference; worth measuring rather than presupposing.
- **6-point, 5th-order Hermite/Lagrange.** Two more taps per sample (8 reads instead of 4 across both mips). Audible improvement at large transposition ratios.
- **Olli Niemitalo's optimal polynomial interpolators** ("Polynomial Interpolators for High-Quality Resampling of Oversampled Audio"). Same arithmetic structure as Hermite but with coefficients tuned for minimal aliasing rather than maximal smoothness. Same cost as Hermite; strictly better aliasing behavior. Likely the highest-value swap if v1's aliasing tests reveal headroom.
- **Cubic B-spline.** Smoother but introduces measurable amplitude attenuation at high frequencies. Useful when smoothness is the priority over fidelity (e.g., morphing slow LFO-style wavetables).
- **Windowed sinc (e.g., Lanczos-3 or -5).** Closest to ideal reconstruction. 6–10 taps per sample; a real CPU hit for polyphonic use, but viable for solo voices.
- **All-pass fractional delay.** Very flat magnitude response; introduces phase distortion that's typically inaudible on wavetable signals.

#### 6.5.2 Mip-boundary crossfade

§6.3 step 5 currently uses *linear* `mipFrac` blending: `output = low + (high - low) * mipFrac`. Cedar's house style for crossfade elsewhere is **equal-power** (`cedar/include/cedar/vm/crossfade_state.hpp`), so even the v1 default deserves scrutiny. Candidates:

- **Equal-power crossfade** (`cos(t * π/2)` / `sin(t * π/2)`). Matches cedar's convention; avoids the ~3 dB dip of linear crossfade at the midpoint when the two mips are uncorrelated.
- **Smoothstep / S-curve crossfade.** Slower transition near the boundaries, faster in the middle. Reduces perceptual edge artifacts but slightly widens the audible blend region.
- **Hysteresis on mip selection.** When `freq` jitters around an octave boundary, the fractional mip can chatter. Adding ~5% hysteresis (use `mipFractional - 0.05` going up, `+0.05` going down) eliminates the chatter at the cost of a slightly delayed transition.
- **Oversampled boundary blend.** Run interpolation at 2× internally, then downsample, only across the actual blend region. Eliminates intermodulation between the two mips at the cost of a brief CPU spike.
- **Adaptive blend width.** Currently `mipFrac` ramps over a full octave; narrowing the blend region (e.g., only the inner 30% of each octave) lets each mip play full-bandwidth most of the time. Better quality at boundaries when band-limiting is the bottleneck; worse if mips have audibly different character.

#### 6.5.3 Frame morphing

Linear time-domain blend between adjacent frames (§6.3 step 5) is the simplest morph, but it's also the source of the classic wavetable artifact: when two frames have similar spectra at slightly different phases, the morph produces comb-filter cancellation that audibly thins the sound. This is the single largest perceived-quality difference between v1 and high-end wavetable synths (Serum, Vital, Phase Plant). Candidates, increasingly ambitious:

- **Equal-power morph** (`cos`/`sin` weighting). Same fix as §6.5.2 — avoids the level dip when adjacent frames are uncorrelated. Cheapest possible improvement.
- **Phase-aligned morph.** During preprocessing, FFT-rotate each frame so its fundamental crosses zero at index 0. Time-domain blends between phase-aligned frames have far less cancellation. Costs nothing at runtime; modest preprocessor work; this is what "well-behaved" Serum-style wavetables already do implicitly via authoring convention.
- **Spectral magnitude/phase interpolation.** Already listed as a v2 non-goal in §2.2 — the brief is: store frames in the frequency domain and interpolate magnitudes (and unwrapped phases) between them, then IFFT once per block. Eliminates cancellation entirely at the cost of a per-block (or per-N-samples) FFT and a much more complex audio path. Listed here for completeness; deferral rationale belongs in §2.2 where it already is.
- **Cosine / smoothstep crossfade curve** (independent of the preceding two — pick a curve, then pick a domain). Same considerations as §6.5.2.
- **Zero-crossing-aligned morph.** Only allow the morph index to advance at zero crossings of either frame. Reduces clicks but introduces aliasing-like quantization on the morph axis.
- **Per-harmonic morph.** Decompose each frame into harmonics offline; at runtime, interpolate harmonic amplitudes and resynthesize. Equivalent in spirit to spectral morph but cheaper if the harmonic count is small. Loses inharmonic content (same caveat as §5.3's additive option).

This whole subsection is **deliberately deferred**. v1 ships with linear-on-everything (interpolation kernel: 4-point Hermite; mip crossfade: linear; frame morph: linear time-domain blend). The §12 tests give us baselines (#3/#4 for interpolation+aliasing, #4 for mip crossfade, #5 for frame morph continuity). Subjective listening on Serum-format banks — particularly ones with rich morph trajectories — will guide which subsection to attack first. The most likely high-impact pickups, ordered by expected return on engineering effort: equal-power weighting on both crossfade and morph (cheap), Niemitalo-optimal interpolation kernel (drop-in), phase-aligned morph (preprocessor-only), then spectral morph (v2).

---

## 7. Akkado Integration

### 7.1 Surface syntax

Two builtins. `wt_load` registers a bank; `smooch` (with aliases `wt`, `wave`, `wavetable`) reads it.

```akkado
// 1. Load a bank once at the top of the patch
wt_load("Basic Shapes", "wavetables/basic_shapes.wav")

// 2. Use the active bank
freq     = param("freq",  220, 20, 2000)
tablePos = param("morph",   0,  0, 31)        // 32-frame bank, indexed 0..31

// All four spellings produce the same OSC_WAVETABLE opcode:
sound = smooch(freq, 0, tablePos)
//    = wt(freq, 0, tablePos)
//    = wave(freq, 0, tablePos)
//    = wavetable(freq, 0, tablePos)

sound |> lp(%, 4000, 0.7) |> out(%, %)
```

`tablePos` modulation, e.g. with an LFO:
```akkado
wt_load("Basic Shapes", "wavetables/basic_shapes.wav")
morph = osc("sin", 0.2) * 0.5 + 0.5         // [0, 1]
sound = smooch(220, 0, morph * 31)
sound |> out(%, %)
```

### 7.2 Implicit bank reference (v1 single-bank model)

Per the design choice in this PRD, **there is at most one active wavetable bank**. `wt_load` replaces the active bank (any in-flight notes continue using the *previous* bank for their remaining lifetime via shared-ptr ownership; no audible glitch). Subsequent `smooch(...)` calls read the new bank.

Multi-bank support (named bank IDs as a 4th param) is a v2 follow-up and is explicitly out of scope for this PRD.

### 7.3 Akkado builtin registration

In `akkado/include/akkado/builtins.hpp`:

```cpp
{"smooch",    {Opcode::OSC_WAVETABLE, 3, 0, true,
               {"freq", "phase", "tablePos", "", ""},
               {NaN, 0.0f, 0.0f, NaN, NaN}, ...}},
{"wt",        /* same entry, alias */ },
{"wave",      /* same entry, alias */ },
{"wavetable", /* same entry, alias */ },

{"wt_load",   /* host-time-only builtin: takes (string name, string path);
                 emits no opcode, runs at compile/load time */ },
```

---

## 8. Edge Cases

Explicit decisions for every edge case the implementer would otherwise have to guess:

| # | Case                                              | Behavior                                                                                  |
| - | ------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| a | `freq <= 0`                                       | Clamp to `0.01 Hz`. (Negative freqs would invert phase direction; not supported in v1.)   |
| b | `freq >= sampleRate / 2`                          | Clamp to `sampleRate * 0.5 - 1`. Mip selection produces level 10 (fundamental only).      |
| c | No bank loaded (registry empty)                   | Output silence (`0.0`). Do not crash. Issue one log warning per program load.             |
| d | Bank with zero frames                             | Output silence. (Should never happen — `wt_load` rejects empty WAVs at load time.)        |
| e | Bank with one frame                               | Use that frame for both `frameA` and `frameB`; morph blend collapses to identity.         |
| f | `tablePos < 0` or `tablePos >= frames.size()`     | Clamp to `[0, frames.size() - 1]`. No wrap (would cause discontinuity at end of bank).    |
| g | `tablePos` is NaN                                 | Treat as `0`.                                                                             |
| h | End-of-bank `frameB`                              | Clamp to `frames.size() - 1` (so `frameA = frameB` at the last frame; morph collapses).   |
| i | Modulating freq across mip-level boundary         | Fractional mip + crossfade (§6.3 step 2) makes the transition smooth.                     |
| j | Audio-rate vs scalar inputs                       | All three primary inputs are audio-rate buffers; constants use Cedar's constant-buffer.   |
| k | Hot-swap: same `OSC_WAVETABLE` node before/after  | Phase preserved via Cedar's semantic-ID matching; bank reference re-resolved from registry. |
| l | Hot-swap: bank reloaded with different frame count | New voices pick up the new bank; old voices continue with their captured shared-ptr.      |
| m | WAV file size not a multiple of 2048              | `wt_load` fails fast with a clear error message; previous active bank remains.             |
| n | WAV is stereo                                     | `wt_load` fails fast with a clear error. (Wavetables are mono-only by convention.)        |

---

## 9. What Stays vs. What Changes

| Component                          | Status                                                                       |
| ---------------------------------- | ---------------------------------------------------------------------------- |
| Existing oscillators (SIN/SAW/...) | **Unchanged**. Wavetables are additive.                                       |
| `OscState` struct                  | **Unchanged**. Wavetable uses new `SmoochState`.                              |
| `cedar::WavLoader`                 | **Reused as-is** for loading wavetable WAV files.                             |
| Polyphony (`POLY` opcode)          | **Reused as-is**. Each voice gets its own `SmoochState` via `state_id`.       |
| FFT API (`cedar/dsp/fft.hpp`)      | **Unchanged**. Wavetable preprocessor calls `kiss_fftr` directly.             |
| `Instruction` format               | **Unchanged**. Wavetable uses 3 of 5 input slots.                              |
| Hot-swap state preservation        | **Unchanged**. Wavetable benefits from existing semantic-ID matching.         |
| **NEW:** `OSC_WAVETABLE` opcode    | Added to `cedar/include/cedar/vm/instruction.hpp`.                            |
| **NEW:** `WavetableBank` resource  | Host-side, lives outside the arena.                                           |
| **NEW:** `WavetableBankRegistry`   | Singleton, swapped atomically; one active bank in v1.                         |
| **NEW:** `wt_load`, `smooch` and aliases | Akkado builtins.                                                         |
| **NEW:** `experiments/test_op_smooch.py` | Python opcode test per `dsp-experiment-methodology.md`.                |

---

## 10. File Change Inventory

| File                                                          | Action                                         |
| ------------------------------------------------------------- | ---------------------------------------------- |
| `cedar/include/cedar/vm/instruction.hpp`                      | **Modify** — add `OSC_WAVETABLE` enum value.   |
| `cedar/include/cedar/opcodes/oscillators.hpp`                 | **Modify** — add `OSC_WAVETABLE` opcode body.  |
| `cedar/include/cedar/opcodes/dsp_state.hpp`                   | **Modify** — add `SmoochState`.                |
| `cedar/include/cedar/wavetable/bank.hpp` (new)                | **Create** — `WavetableBank`, `WavetableFrame`. |
| `cedar/include/cedar/wavetable/registry.hpp` (new)            | **Create** — `WavetableBankRegistry`.           |
| `cedar/src/wavetable/preprocessor.cpp` (new)                  | **Create** — offline FFT mip generation.       |
| `cedar/src/wavetable/registry.cpp` (new)                      | **Create** — singleton + atomic swap.           |
| `cedar/include/cedar/vm/execution_context.hpp`                | **Modify** — add `wavetable_bank` cached ptr.   |
| `akkado/include/akkado/builtins.hpp`                          | **Modify** — register `smooch`, `wt`, `wave`, `wavetable`, `wt_load`. |
| `akkado/src/codegen.cpp`                                      | **Modify** — emit `OSC_WAVETABLE`; handle `wt_load` as host-time call. |
| `web/wasm/nkido_wasm.cpp`                                     | **Modify** — expose `wt_load` via WASM bridge.  |
| `experiments/test_op_smooch.py` (new)                         | **Create** — opcode test (§12).                |
| `cedar/include/cedar/generated/opcode_metadata.hpp`           | **Regen** via `bun run build:opcodes`.         |
| `docs/dsp-quality-checklist.md`                               | **Modify** — add wavetable test coverage row.   |

---

## 11. Phased Implementation Plan

### Phase 1 — Bank + offline preprocessor
- `WavetableBank`, `WavetableFrame`, `SmoochState` types.
- `cedar/src/wavetable/preprocessor.cpp` with raised-cosine taper + peak normalization.
- Unit tests for preprocessor: full round-trip on known shapes (sine, saw, square), verify peak preservation, verify aliasing-rejection at each mip level.

### Phase 2 — `OSC_WAVETABLE` opcode + StatePool wiring
- Enum value, opcode body, state struct.
- `WavetableBankRegistry` with atomic shared-ptr swap.
- `ExecutionContext::wavetable_bank` cached pointer.
- Unit tests for the opcode in isolation (no Akkado): construct a bank in code, run the opcode at fixed freqs, measure THD and aliasing.

### Phase 3 — Akkado integration
- Register `smooch`, `wt`, `wave`, `wavetable` builtins (all aliasing `OSC_WAVETABLE`).
- Register `wt_load` as a host-time builtin (parsed at compile time, executed against the registry).
- Codegen: emit the opcode with correct input slots; route `wt_load` to the registry.
- Web/WASM bridge for `wt_load` (file path resolution differs native vs web — reuse SoundFont loading model).

### Phase 4 — Hot-swap, polyphony, and quality
- Verify state preservation across reloads (semantic-ID matching).
- Verify `poly(smooch(...), N)` produces N independent voices reading the same shared bank.
- DSP quality pass: long render, aliasing measurement, level matching across mip boundaries, morphing continuity.

Each phase ends with the tests from §12 for that phase's scope passing.

---

## 12. Testing Strategy

Per `docs/dsp-experiment-methodology.md` and CLAUDE.md, opcode tests live in `experiments/test_op_smooch.py` and follow the standard test host pattern. Stateful opcodes that drive sequences must simulate **at least 300 seconds of audio**.

### 12.1 Test cases (each with explicit pass/fail criteria)

| #  | Test                                       | Pass criterion                                                                         |
| -- | ------------------------------------------ | -------------------------------------------------------------------------------------- |
| 1  | Round-trip: sine at 440 Hz                 | THD < -60 dB; output peak within 0.5 dB of source.                                     |
| 2  | Aliasing sweep: 20 Hz → Nyquist over 10 s  | No spectral content above Nyquist at any point. WAV output for human listening.        |
| 3  | Aliasing at high pitch: 10 kHz fundamental | No alias bin above noise floor (-90 dB).                                                |
| 4  | Mip-level boundary crossfade               | Smooth amplitude during a slow freq sweep across each octave boundary; no clicks.      |
| 5  | Frame morph continuity                     | Sweep `tablePos` 0 → numFrames over 10 s; no zipper noise; no discontinuities.         |
| 6  | Single-frame bank                          | Opcode produces non-silent output; tablePos input ignored without crashing.            |
| 7  | Empty registry                             | Opcode produces silent output; no crash; one warning logged.                            |
| 8  | tablePos out of range / NaN                | Clamping behavior matches §8 spec; no crash; no NaN in output buffer.                   |
| 9  | Long pattern run (≥ 300 s)                 | No state drift, no allocation, no audio dropouts; final RMS within 1 dB of initial.    |
| 10 | Hot-swap: edit code, keep `smooch` node    | Phase preserved; no audible click at edit boundary.                                     |
| 11 | Hot-swap: `wt_load` swaps bank             | New voices read new bank; existing voices continue with old bank; no glitch.            |
| 12 | Memory budget                              | A 256-frame bank loads under 25 MB resident; rejected above the soft 32 MB limit.       |

### 12.2 Reference WAV outputs

Every test must save a WAV file to `experiments/output/op_smooch/` with a description of what to listen for, per CLAUDE.md's testing guidelines.

### 12.3 Quality checklist update

After Phase 4 ships, update `docs/dsp-quality-checklist.md` with the wavetable test coverage row.

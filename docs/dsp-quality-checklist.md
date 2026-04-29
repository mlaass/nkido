> **Status: TRACKING** — 92% opcode coverage.

# DSP Quality Assurance Checklist

This document tracks the quality verification status of Cedar DSP opcodes. Each opcode should be tested for correctness, performance, and audio quality before being considered production-ready.

## Quality Criteria by Category

### Oscillators
- **Frequency accuracy**: Output frequency matches input within 0.1%
- **Aliasing**: High frequencies fold below Nyquist without audible artifacts
- **DC offset**: Output centered around zero (< 0.001 DC component)
- **Harmonic purity**: Spectrum matches theoretical waveform harmonics
- **Phase continuity**: No discontinuities during frequency modulation

### Filters
- **Frequency response**: Cutoff matches specification within 1dB
- **Resonance behavior**: Q factor produces expected peak
- **Self-oscillation**: High resonance produces clean sine (where applicable)
- **Stability**: No blowups at extreme settings

### Effects
- **Wet/dry blend**: Clean signal path when dry
- **Modulation smoothness**: No zipper noise on parameter changes
- **Impulse response**: Decay characteristics match design

### Envelopes
- **Timing accuracy**: Attack/decay/release times match within 1% (or ±5 samples)
- **Curve shape**: Exponential vs linear matches specification
- **Retrigger behavior**: Handles rapid triggers without glitches
- **Sample accuracy**: All timing parameters read from buffer inputs for per-sample precision

### Dynamics
- **Threshold accuracy**: Compression/gating activates at specified level
- **Attack/release**: Timing matches specification
- **Gain reduction**: Ratio produces expected output levels

---

## Oscillators

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `OSC_SIN` | ✅ Tested | Frequency accuracy, FM aliasing | Basic sine, good quality |
| `OSC_SIN_4X` | ✅ Tested | 4x oversampling FM test | Minimal aliasing |
| `OSC_TRI` | ✅ Tested | Waveform shape, harmonic content | PolyBLEP anti-aliasing |
| `OSC_SAW` | ✅ Tested | Waveform, harmonics, aliasing | PolyBLEP anti-aliasing |
| `OSC_SQR` | ✅ Tested | DC offset, harmonics, PolyBLEP comparison | Extensive quality tests |
| `OSC_SQR_MINBLEP` | ✅ Tested | Compared against PolyBLEP | Higher quality, more CPU |
| `OSC_SQR_PWM` | ✅ Tested | Duty cycle accuracy (10%-90%) | PWM range verified |
| `OSC_SQR_PWM_4X` | ✅ Tested | PWM + FM aliasing | 4x oversampling |
| `OSC_SAW_PWM` | ✅ Tested | Waveform shapes, frequency accuracy, PWM sweep spectrogram | PolyBLAMP, dedicated test file |
| `OSC_SAW_PWM_4X` | ✅ Tested | Aliasing comparison vs 1x, high-frequency quality | 4x oversampled, dedicated test file |
| `OSC_SQR_PWM_MINBLEP` | ✅ Tested | Duty cycle sweep, aliasing vs PolyBLEP, high-frequency quality | MinBLEP sub-sample accuracy |
| `OSC_RAMP` | ✅ Tested | Waveform shape, frequency accuracy, DC offset, PolyBLEP aliasing, inverted-SAW equivalence | Descending ramp with PolyBLEP |
| `OSC_PHASOR` | ✅ Tested | Range [0,1), linearity (R²=1.0), frequency accuracy, phase increment, waveshaping | Raw phase output |
| `OSC_WAVETABLE` | ✅ Tested | THD (-101 dB at 440 Hz sine), alias floor (-103 dB at 10 kHz saw), mip-boundary crossfade, frame morph continuity, NaN/OOR tablePos, empty-registry silence, single-frame bank, 305 s long-run stability, 256-frame bank memory | Smooch — Niemitalo-4 + equal-power mip/frame crossfade. Hot-swap tests #10/#11 pending CLI smoke harness. |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `OSC_SAW_4X` | Low | 4x oversampling, FM test |
| `OSC_SQR_4X` | Low | 4x oversampling, FM test |
| `OSC_TRI_4X` | Low | 4x oversampling, FM test |

---

## Filters

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `FILTER_SVF_LP` | ✅ Tested | Frequency response, resonance sweep | State-variable lowpass |
| `FILTER_SVF_HP` | ✅ Tested | Frequency response, resonance sweep | State-variable highpass |
| `FILTER_SVF_BP` | ✅ Tested | Frequency response, resonance sweep | State-variable bandpass |
| `FILTER_MOOG` | ✅ Tested | Resonance sweep, self-oscillation | Classic ladder character |
| `FILTER_DIODE` | ✅ Tested | Frequency response, resonance sweep, self-oscillation, Moog comparison | ZDF diode ladder (TB-303 style) |
| `FILTER_FORMANT` | ✅ Tested | Vowel accuracy (A/I/U/E/O), morph smoothness spectrogram | 3-band parallel vowel filter |
| `FILTER_SALLENKEY` | ✅ Tested | LP/HP modes, self-oscillation, diode character analysis | MS-20 style with diode feedback |

*All implemented filter opcodes are now tested.*

---

## Effects

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `DISTORT_TANH` | ✅ Tested | Transfer curve | Soft clipping saturation |
| `DISTORT_SOFT` | ✅ Tested | Transfer curve | Gentle saturation |
| `DISTORT_FOLD` | ✅ Tested | Transfer curve, ADAA aliasing analysis, symmetry effect, continuity | Sine wavefolder with ADAA antialiasing |
| `DISTORT_TUBE` | ✅ Tested | Transfer curve | Tube-style harmonics |
| `DISTORT_BITCRUSH` | ✅ Tested | Bit depth levels, sample rate reduction | Quantization + aliasing |
| `EFFECT_PHASER` | ✅ Tested | Spectrogram sweep | 6-stage phaser |
| `EFFECT_CHORUS` | ✅ Tested | Spectral spread, sideband analysis | 0.71 spread ratio |
| `EFFECT_FLANGER` | ✅ Tested | Spectrogram sweep pattern | Comb filter notches |
| `DISTORT_SMOOTH` | ✅ Tested | Transfer curve (tanh shape), drive sweep THD, ADAA antialiasing comparison | ADAA first-order |
| `DISTORT_TAPE` | ✅ Tested | Saturation curve, warmth HF rolloff (spectral centroid), drive scaling | 2x oversampled |
| `DISTORT_XFMR` | ✅ Tested | Bass emphasis THD comparison, bass_freq crossover, drive harmonics | Transformer bass saturation |
| `DISTORT_EXCITE` | ✅ Tested | Passthrough at amount=0, harmonic generation above freq, odd/even balance | Aural exciter |
| `EFFECT_COMB` | ✅ Tested | Resonance frequency peaks, positive/negative feedback, damping | Feedback comb filter |

---

## Delays & Reverbs

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `DELAY` | ✅ Tested | Delay time accuracy (0.00% error), feedback decay (-6dB/echo) | 4800 samples @ 100ms |
| `REVERB_DATTORRO` | ✅ Tested | Impulse response, decay analysis | Long tail reverb |
| `REVERB_FREEVERB` | ✅ Tested | Impulse response, room_size RT60 scaling, damping spectral centroid | Schroeder-Moorer |
| `REVERB_FDN` | ✅ Tested | Impulse response, decay RT60 scaling, damping HF rolloff | 4x4 Hadamard FDN |
| `DELAY_TAP/WRITE` | ✅ Tested | Timing accuracy (ms), feedback decay, time unit modes (s/ms/samples) | Shared-state tap delay pair |

---

## Samplers

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `SAMPLE_PLAY` | ✅ Tested | Pitch accuracy, interpolation, timing | One-shot playback |
| `SAMPLE_PLAY_LOOP` | ✅ Tested | Seamless looping, pitch shifting, gate control, loop boundary analysis | Dedicated looping test file |

---

## Envelopes

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `ENV_ADSR` | ✅ Tested | Timing accuracy (<1% error), curve shape, gate behavior, retrigger, sample-accurate release (10ms-500ms) | Full ADSR lifecycle, 5-input format with buffer-based release |
| `ENV_AR` | ✅ Tested | Attack/release timing (<1% error), curve shape, sample accuracy | Simplified envelope, ±5 samples precision |
| `ENV_FOLLOWER` | ✅ Tested | Attack/release response, signal tracking | Amplitude following |

---

## Sequencers & Timing

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `CLOCK` | ✅ Tested | Phase accuracy, BPM sync, long-term drift | 0 samples drift over 100 beats |
| `LFO` | ✅ Tested | All shapes, frequency sync, PWM duty, zero-crossing precision | Direct phase calculation, 0 drift |
| `EUCLID` | ✅ Tested | Pattern accuracy, step timing precision | 0 samples timing error |
| `TRIGGER` | ✅ Tested | Division accuracy, long-term precision, cross-opcode alignment | ≤1 sample error over 1000 beats |
| `TIMELINE` | ⚠ Partial | Zero-breakpoint fallback, stability (no state population API in Python) | Needs bindings extension |

---

## Dynamics

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `DYNAMICS_COMP` | ✅ Tested | Transfer curve, threshold -20dB, ratio 4:1, max error 1.7dB | Compressor working |
| `DYNAMICS_LIMITER` | ✅ Tested | Ceiling enforcement, transient handling, 0dB overshoot | Limiter working |
| `DYNAMICS_GATE` | ✅ Tested | Passes loud signals, hysteresis OK, attenuation speed verified | Fast 5ms gate close, full attenuation within 200ms |

---

## Utility

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `NOISE` | ✅ Tested | Distribution (uniform), mean/std 0.577, spectral flatness 0.02dB variance | White noise generator |
| `MTOF` | ✅ Tested | MIDI to frequency accuracy (0-127 range), <0.00002% error | Note to Hz conversion |
| `SAH` | ✅ Tested | Trigger timing, hold value stability | Sample-and-hold working |
| `SLEW` | ✅ Tested | Rise/fall timing, rate limiting | <0.1% timing error, sample-accurate |
| `DC` | ✅ Tested | Constant offset accuracy | Fixed memcpy bug (was reading 32-bit from 16-bit field) |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `ENV_GET` | Low | Envelope value extraction |

---

## Test Coverage Summary

| Category | Tested | Partial | Untested | Notes |
|----------|--------|---------|----------|-------|
| Oscillators | 13 | - | 3 | Added `test_op_ramp.py`, `test_op_phasor.py` |
| Filters | 7 | - | 0 | `test_op_lp.py`, `test_op_hp.py`, `test_op_bp.py`, `test_op_moog.py`, etc. |
| Effects | 13 | - | 0 | Added `test_op_smooth.py`, `test_op_tape.py`, `test_op_xfmr.py`, `test_op_excite.py`, `test_op_comb.py` |
| Delays & Reverbs | 5 | - | 0 | Added `test_op_freeverb.py`, `test_op_fdn.py`, `test_op_tap_delay.py` |
| Samplers | 2 | - | 0 | Added dedicated `test_op_sample_loop.py` |
| Envelopes | 3 | - | 0 | `test_op_adsr.py`, `test_op_ar.py`, `test_op_env_follower.py` |
| Sequencers & Timing | 4 | 1 | 0 | Added `test_op_timeline.py` (partial: no state API) |
| Dynamics | 3 | - | 0 | `test_op_comp.py`, `test_op_limiter.py`, `test_op_gate.py` |
| Utility | 5 | - | 1 | `test_op_noise.py`, `test_op_mtof.py`, `test_op_sah.py`, `test_op_slew.py` |
| **Total** | **55** | **2** | **4** | 92% tested |

---

## Priority Action Items

### High Priority - Extend Python Bindings
1. `TIMELINE` - Add Python API to populate `TimelineState` for full testing

### Low Priority (Completeness)
1. 4x oversampling variants for SAW, SQR, TRI
2. Utility opcodes (`ENV_GET`)

---

## Not Implemented / Planned

These opcodes are not yet implemented but may be added in the future.

### Filters

| Opcode | Notes |
|--------|-------|
| `FILTER_BIQUAD_LP` | Standard biquad lowpass (deprecated in favor of SVF) |
| `FILTER_BIQUAD_HP` | Standard biquad highpass |
| `FILTER_BIQUAD_BP` | Standard biquad bandpass |
| `FILTER_BIQUAD_NOTCH` | Notch filter |
| `FILTER_BIQUAD_PEAK` | Peaking EQ |
| `FILTER_BIQUAD_LSHELF` | Low shelf EQ |
| `FILTER_BIQUAD_HSHELF` | High shelf EQ |
| `FILTER_COMB` | Comb filter (feedforward) |
| `FILTER_ALLPASS` | Allpass filter |

### Reverbs

| Opcode | Notes |
|--------|-------|
| `REVERB_SPRING` | Spring reverb emulation |
| `REVERB_SHIMMER` | Pitch-shifted reverb |

### Samplers

| Opcode | Notes |
|--------|-------|
| `SAMPLE_GRANULAR` | Granular synthesis |
| `SAMPLE_KARPLUS` | Karplus-Strong string synthesis |

### Synths

| Opcode | Notes |
|--------|-------|
| `SMOOCH` | Wavetable oscillator with mip-mapped anti-aliasing and Hermite interpolation. See [PRD](prd-smooch-wavetable-synth.md) |

---

## Testing Guidelines

### Creating New Tests

Tests are Python scripts in `experiments/` following the one-file-per-opcode pattern. See [DSP Experiment Methodology](dsp-experiment-methodology.md) for the full guide.

```bash
# Create a new test
experiments/test_op_<codename>.py

# Run it
cd experiments && uv run python test_op_<codename>.py

# Run all tests
cd experiments && ./run_all.sh
```

Each test file uses `CedarTestHost` from `cedar_testing.py` and writes output to `experiments/output/op_<codename>/`.

### Analysis Tools Available
- FFT spectrum analysis (numpy/scipy)
- DC offset measurement
- RMS level calculation
- Frequency response / Bode plots (`filter_helpers.py`)
- Spectrograms and transfer curves (`visualize.py`)
- WAV file export for auditory verification

### Naming Convention
- Test files: `test_op_<codename>.py` (e.g., `test_op_lp.py`, `test_op_fold.py`)
- Output dirs: `output/op_<codename>/`
- Test functions: `test_<aspect>()` (e.g., `test_svf_lp_cutoff_sweep()`)

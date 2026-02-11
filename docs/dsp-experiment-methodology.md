> **Status: REFERENCE** — Python testing methodology. Active.

# DSP Experiment Methodology

This document explains how we verify the quality and correctness of Cedar DSP opcodes using Python experiments.

## Purpose

Cedar's DSP opcodes implement audio algorithms (oscillators, filters, effects, etc.) that must sound correct to human ears. Automated unit tests catch regressions, but audio quality requires **human-in-the-loop verification**: listening to WAV outputs and inspecting spectrograms.

The Python experiment framework provides:
- A lightweight harness to drive the Cedar VM via `cedar_core` bindings
- WAV and PNG output for human evaluation
- Quantitative metrics (frequency accuracy, timing error, spectral analysis) as sanity checks

## What We Test

### Correctness
- **Frequency accuracy**: oscillator output matches requested frequency within 0.1%
- **Timing precision**: envelopes, clocks, and sequencers hit their marks within ±5 samples
- **Transfer curves**: distortion and dynamics processors match their mathematical definitions
- **Filter response**: cutoff frequency and resonance match specification within 1dB

### Quality
- **Aliasing**: high-frequency content folds below Nyquist without audible artifacts
- **Artifacts**: no clicks, pops, zipper noise, or DC offset
- **Noise floor**: signal-to-noise ratio appropriate for the algorithm
- **Stability**: no blowups at extreme parameter settings

### Musicality (listen tests)
- Does the filter self-oscillate cleanly?
- Does the reverb tail sound natural?
- Does the chorus produce pleasing detuning?
- Are envelope curves musically useful?

These questions can only be answered by listening to the WAV output.

## Quality Criteria by Category

### Oscillators
- **Frequency accuracy**: output frequency matches input within 0.1%
- **Aliasing**: high frequencies fold below Nyquist without audible artifacts
- **DC offset**: output centered around zero (< 0.001 DC component)
- **Harmonic purity**: spectrum matches theoretical waveform harmonics
- **Phase continuity**: no discontinuities during frequency modulation

### Filters
- **Frequency response**: cutoff matches specification within 1dB
- **Resonance behavior**: Q factor produces expected peak
- **Self-oscillation**: high resonance produces clean sine (where applicable)
- **Stability**: no blowups at extreme settings

### Effects
- **Wet/dry blend**: clean signal path when dry
- **Modulation smoothness**: no zipper noise on parameter changes
- **Impulse response**: decay characteristics match design

### Envelopes
- **Timing accuracy**: attack/decay/release times match within 1% (or ±5 samples)
- **Curve shape**: exponential vs linear matches specification
- **Retrigger behavior**: handles rapid triggers without glitches
- **Sample accuracy**: all timing parameters read from buffer inputs for per-sample precision

### Dynamics
- **Threshold accuracy**: compression/gating activates at specified level
- **Attack/release**: timing matches specification
- **Gain reduction**: ratio produces expected output levels

## Test Categories

| Category | Opcodes | Example Files |
|----------|---------|---------------|
| Oscillators | SIN, TRI, SAW, SQR, PWM variants | `test_op_osc.py`, `test_op_osc_fm.py`, `test_op_sqr_*.py` |
| Filters | SVF LP/HP/BP, Moog, Diode, Sallen-Key, Formant | `test_op_lp.py`, `test_op_moog.py`, `test_op_diode.py` |
| Effects | Chorus, Flanger, Phaser, Delay, Ping-pong | `test_op_chorus.py`, `test_op_delay.py`, `test_op_phaser.py` |
| Distortion | Saturate, Softclip, Fold, Tube, Bitcrush | `test_op_fold.py`, `test_op_bitcrush.py`, `test_op_tube.py` |
| Envelopes | ADSR, AR, Env Follower | `test_op_adsr.py`, `test_op_ar.py`, `test_op_env_follower.py` |
| Dynamics | Compressor, Limiter, Gate | `test_op_comp.py`, `test_op_limiter.py`, `test_op_gate.py` |
| Sequencers | Clock, LFO, Euclidean, Trigger | `test_op_clock.py`, `test_op_lfo.py`, `test_op_euclid.py` |
| Stereo | Pan, Width, M/S Encode/Decode | `test_op_pan.py`, `test_op_width.py`, `test_op_ms_encode.py` |
| Utility | Noise, MTOF, Sample & Hold, Slew | `test_op_noise.py`, `test_op_mtof.py`, `test_op_sah.py` |
| Samplers | Sample playback | `test_op_sample.py` |
| Reverbs | Dattorro | `test_op_dattorro.py` |

## Output Types

Each test writes to `experiments/output/op_<codename>/`:

- **WAV files** — primary output for human listening. Every test must produce at least one WAV.
- **PNG files** — spectrograms, frequency response plots, transfer curves. Generated via `visualize.py`.
- **Console output** — quantitative pass/fail with ✓/✗/⚠ symbols and measured values.

## How to Add a New Test

1. **Create the file**: `experiments/test_op_<codename>.py` where `<codename>` matches the opcode's short name (e.g., `lp` for `FILTER_SVF_LP`).

2. **Use the standard structure**:
   ```python
   """
   Test: <Opcode Name> (<OPCODE_ENUM>)
   ====================================
   Tests [what it does] at [various conditions].

   Expected behavior:
   - [criterion 1]
   - [criterion 2]

   If this test fails, check the implementation in cedar/include/cedar/opcodes/<file>.hpp
   """

   import os
   import numpy as np
   import scipy.io.wavfile
   from cedar_testing import CedarTestHost, output_dir

   OUT = output_dir("op_<codename>")


   def test_something():
       """Test OPCODE for [specific behavior]."""
       print("Test: <Opcode> - <Aspect>")
       host = CedarTestHost()
       # ... set up instructions, process blocks ...

       # Always save WAV
       wav_path = os.path.join(OUT, "test_something.wav")
       scipy.io.wavfile.write(wav_path, host.sr, output)
       print(f"  Saved {wav_path} - Listen for [what to hear]")

       # Report pass/fail
       if meets_criteria:
           print(f"  ✓ PASS: [what passed]")
       else:
           print(f"  ✗ FAIL: [what failed] - Check implementation")


   if __name__ == "__main__":
       test_something()
   ```

3. **Use shared modules**:
   - `cedar_testing.py` — `CedarTestHost` class and `output_dir()` helper
   - `filter_helpers.py` — `analyze_filter()`, `get_bode_data()`, `get_impulse()` for filter tests
   - `visualize.py` — `save_figure()` for consistent PNG output
   - `utils.py` — general utilities

4. **Run it**: `cd experiments && uv run python test_op_<codename>.py`

5. **Update the checklist**: add the opcode to `docs/dsp-quality-checklist.md`

## When a Test Fails

1. Do NOT modify the test to make it pass
2. Investigate the C++ implementation
3. Discuss with the team whether the algorithm needs fixing
4. If the expected behavior was genuinely wrong, update both test AND documentation

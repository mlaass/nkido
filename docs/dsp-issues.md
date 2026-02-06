# DSP Issues Found in Experiments

Tracked issues discovered by opcode experiment tests. Each entry includes the failing test, observed behavior, and suggested investigation path.

---

## Open

### 1. DISTORT_SMOOTH: Non-monotonic transfer curve (ADAA block boundary artifacts)

**Test:** `test_op_smooth.py` → `test_transfer_curve()`
**Severity:** Medium
**Observed:** All drive levels fail strict monotonicity check (`diffs >= -1e-6`). Small negative diffs appear at what appear to be block boundaries during linear ramp processing.
**Expected:** ADAA tanh saturation should produce a monotonically increasing transfer curve for monotonically increasing input.
**Other checks pass:** Bounded output, symmetry, THD monotonicity, antialiasing (9.2dB improvement over naive tanh).
**Investigate:** `cedar/include/cedar/opcodes/distortion.hpp` — ADAA state reset/carry between blocks. First-order ADAA requires the antiderivative value from the previous sample to be carried across block boundaries correctly.

### 2. REVERB_FREEVERB: RT60 estimates at 0.00s for all room sizes

**Test:** `test_op_freeverb.py` → `test_impulse_response()`, `test_room_size()`
**Severity:** Medium
**Observed:** Peak amplitude only 0.0671 for impulse response (room=0.8, damp=0.5, rate=255 fully wet). `estimate_rt60()` returns 0.00s for all room sizes (0.2, 0.5, 0.8, 0.95). However, the damping spectral centroid test passes correctly — the reverb is producing spectrally different output at different damping levels.
**Expected:** Freeverb with room=0.8 should produce a visible reverb tail with RT60 > 0.1s.
**Investigate:** Either the reverb output level is very low (gain staging issue in the comb/allpass chain), or the `room_scale`/`room_offset` parameter values (0.28/0.7) aren't producing sufficient feedback. Compare with the Dattorro reverb test which works. Check `cedar/include/cedar/opcodes/reverbs.hpp`.

### 3. REVERB_FDN: RT60 estimates at 0.00s for all decay values

**Test:** `test_op_fdn.py` → `test_impulse_response()`, `test_decay_parameter()`
**Severity:** Medium
**Observed:** Peak amplitude 0.175 for impulse response. RT60=0.00s for all decay values (0.3, 0.6, 0.8, 0.95). Damping spectral centroid test passes (12037Hz → 223Hz as damping increases).
**Expected:** FDN with decay=0.8 should produce a reverb tail with measurable RT60.
**Investigate:** Same pattern as Freeverb — output exists but decays too quickly for RT60 estimation. May be a gain issue in the Hadamard feedback matrix, or the `estimate_rt60` smoothing window (10ms) may be too coarse for the signal level. Check `cedar/include/cedar/opcodes/reverbs.hpp`.

### 4. SAMPLE_PLAY_LOOP: Click at loop boundary

**Test:** `test_op_sample_loop.py` → `test_seamless_looping()`
**Severity:** Medium
**Observed:** Max discontinuity at loop wrap point is 0.0576 (threshold: 0.01). Test uses a sine wave sample where loop start/end should be zero-crossing aligned.
**Expected:** Looped playback should be seamless with no audible click at the loop boundary, especially when the sample is designed for perfect looping.
**Other checks pass:** Pitch shifting accurate (<0.73 cents), gate control works (silence during gate-off).
**Investigate:** `cedar/include/cedar/opcodes/samplers.hpp` — check whether the loop interpolation crossfades at the wrap point. The discontinuity suggests either the interpolation reads past the loop end without wrapping, or there's an off-by-one in the loop boundary calculation.

### 5. OSC_SQR_PWM_MINBLEP: Higher noise floor than PolyBLEP

**Test:** `test_op_sqr_pwm_minblep.py` → `test_aliasing_comparison()`
**Severity:** Low
**Observed:** At 440Hz, MinBLEP noise floor is -26.6dB vs PolyBLEP at -64.7dB — MinBLEP is 38dB *worse*. At higher frequencies the gap narrows (8000Hz: -134.1dB vs -135.3dB).
**Expected:** MinBLEP should produce equal or lower aliasing noise than PolyBLEP, particularly at higher frequencies.
**Investigate:** The 440Hz result is suspicious — -26.6dB noise floor is very high and may indicate the MinBLEP residual buffer is not being applied correctly, or there's spectral leakage in the measurement. Listen to WAV output for audible comparison. Check `cedar/include/cedar/opcodes/oscillators.hpp`.

### 6. OSC_SAW_PWM_4X: 4x oversampling not improving at all frequencies

**Test:** `test_op_saw_pwm_4x.py` → `test_aliasing_comparison()`
**Severity:** Low
**Observed:** At 2000Hz: 4x is 1.9dB worse. At 8000Hz: 4x is 11.1dB worse. At 440Hz: 5.6dB improvement. At 5000Hz: 8.0dB improvement.
**Expected:** 4x oversampling should consistently reduce aliasing across all frequencies.
**Investigate:** The inconsistency may be a measurement artifact (noise floor estimation method) or may indicate the decimation filter isn't fully attenuating mirror images at certain frequencies. Check the oversampling implementation and decimation filter in `cedar/include/cedar/opcodes/oscillators.hpp`.

---

## Won't Fix / By Design

*(Move items here if investigation shows the behavior is intentional)*

---

## Resolved

*(Move items here once fixed, with commit reference)*

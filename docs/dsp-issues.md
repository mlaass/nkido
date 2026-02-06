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

### 7. ADSR: Envelope timing completely wrong (attack/decay/release orders of magnitude too fast)

**Test:** `test_op_adsr.py` → `test_stage_timing()`, `test_sample_accurate()`, `test_exponential_curve()`
**Severity:** High
**Observed:** All ADSR timing parameters are ignored. Attack always completes in ~1ms (48 samples = 1 block) regardless of requested time. Decay measured at 0.1ms. Release measured at 0.0ms. Sustain level is 0.0000 instead of expected values (0.3–0.7). Exponential curve checkpoints all measure 0.0000. Only the 1ms attack case passes because it fits within a single block.
**Expected:** ADSR with A=50ms should take ~2400 samples to reach peak. Sustain level should match the S parameter. Release should decay over the requested time.
**Investigate:** `cedar/include/cedar/opcodes/envelopes.hpp` — the envelope coefficient calculation likely has a unit error (e.g., computing per-block instead of per-sample rates), or the time parameters are being interpreted in the wrong units. The fact that gate edge detection works (0-sample delay) suggests the trigger mechanism is fine but the envelope ramp generation is broken.

### 8. EUCLID: Rotation parameter produces wrong patterns

**Test:** `test_op_euclid.py` → `test_rotation()`
**Severity:** Medium
**Observed:** E(3,8) with rot=0 is correct. But rot=1 gives `01001010` instead of expected `10010010`. rot=2 gives `10010100` instead of `01001001`. rot=3 gives `00101001` instead of `10100100`. rot=4 is correct again.
**Expected:** Rotation should cyclically shift the Euclidean pattern by the specified number of steps.
**Investigate:** `cedar/include/cedar/opcodes/sequencers.hpp` — the rotation direction may be inverted or the rotation amount may be applied to the wrong phase of the pattern generator.

### 9. FILTER_DIODE: Self-oscillation fails for most VT/feedback configurations

**Test:** `test_op_diode.py` → `test_self_oscillation()`
**Severity:** Medium
**Observed:** Original (VT=0.026, FB=1.0) correctly does not oscillate. A_fb10 (VT=0.026, FB=10.0) oscillates but at 1359Hz instead of 1000Hz (35.9% error). B_mid (VT=0.05, FB=5.0) produces no oscillation (max amp=0.0). C_soft (VT=0.1, FB=2.5) produces no oscillation (max amp=0.0). Only 1 of 4 configurations behaves as expected.
**Expected:** Diode ladder filter should self-oscillate when feedback is high enough. Higher VT (thermal voltage) with moderate feedback should still produce oscillation.
**Investigate:** `cedar/include/cedar/opcodes/filters.hpp` — the VT parameter may be scaling the feedback too aggressively, or the diode nonlinearity may be dampening the signal below the self-oscillation threshold.

### 10. FILTER_FORMANT: F3 frequency 25% off for vowels I and E

**Test:** `test_op_formant.py` → `test_vowel_response()`
**Severity:** Low
**Observed:** Vowel 'I': F3 target=3000Hz, measured=2227Hz (25.8% error). Vowel 'E': F3 target=2550Hz, measured=1951Hz (23.5% error). F1 and F2 are accurate (<4% error) for all vowels. Vowels A, U, O all pass.
**Expected:** All three formant frequencies should be within 10% of target.
**Investigate:** `cedar/include/cedar/opcodes/filters.hpp` — the third formant filter band may have a bandwidth or gain issue causing the peak to shift downward, or the formant table values for I and E may need adjustment.

### 11. OSC_SQR: Even harmonics present (should be absent for 50% duty cycle)

**Test:** `test_op_osc.py` → harmonic analysis
**Severity:** Low
**Observed:** SQR at 440Hz shows H2 at 2.3dB and H4 at 2.2dB (relative to noise floor). A perfect square wave should have zero even harmonics.
**Expected:** Even harmonics should be at or below the noise floor (~2dB).
**Investigate:** This suggests a tiny DC offset or waveform asymmetry in the PolyBLEP square wave. The even harmonics are very low (at noise floor level), so this may be a measurement sensitivity issue rather than a real problem. Check `cedar/include/cedar/opcodes/oscillators.hpp` for DC offset.

### 12. FILTER_SALLENKEY: No self-oscillation at high resonance

**Test:** `test_op_sallenkey.py` → self-oscillation test
**Severity:** Low
**Observed:** Both LP and HP modes produce max amplitude of 0.0 with high resonance. No oscillation detected.
**Expected:** Sallen-Key topology should self-oscillate when resonance (Q) is driven high enough, similar to other ladder/SVF filters.
**Investigate:** `cedar/include/cedar/opcodes/filters.hpp` — the feedback path or resonance scaling may be insufficient to push the filter into self-oscillation. This may also be by design if the Sallen-Key model is intended to remain stable.

### 13. OSC_SQR_PWM_MINBLEP: Amplitude exceeds ±1.0 (overshoot at PWM transitions)

**Test:** `test_op_sqr_pwm_phase.py` → `test_step_response()`
**Severity:** Medium
**Observed:** OSC_SQR_PWM_MINBLEP max amplitude is 1.1694 (16.9% overshoot), while PolyBLEP and 4X variants stay at 1.0000. The step response test (sudden PWM change) fails due to this overshoot.
**Expected:** Output should be bounded to ±1.0 or at least match the amplitude behavior of the PolyBLEP variant.
**Investigate:** `cedar/include/cedar/opcodes/oscillators.hpp` — the MinBLEP residual table may have Gibbs-like ringing that overshoots at discontinuities. This could be a table design issue (insufficient table length or windowing) or the residual is being applied additively without clamping.

### 14. OSC_SAW_PWM: Large discontinuity during PWM sweep

**Test:** `test_op_saw_pwm.py` → PWM sweep test
**Severity:** Low
**Observed:** Max sample-to-sample jump of 0.458 detected during continuous PWM sweep from -1 to +1. Test notes this may be at a band-limit correction boundary.
**Expected:** PWM sweep should produce smooth morphing from ramp to triangle to saw without large discontinuities.
**Investigate:** `cedar/include/cedar/opcodes/oscillators.hpp` — this likely occurs at the PWM=0 crossing point where the waveform shape changes character. The PolyBLEP correction may need a smooth transition region around PWM=0.

### 15. test_op_osc_oversampling: Reference comparison crashes (array shape mismatch)

**Test:** `test_op_osc_oversampling.py` → `test_compare_with_reference()`
**Severity:** Low (test bug)
**Observed:** `ValueError: operands could not be broadcast together with shapes (384,) (480,)`. The reference array is 480 samples (`int(0.01 * 48000)`) but the VM produces 384 samples (3 blocks × 128). `num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)` truncates to 3 instead of ceiling to 4.
**Fix:** Use `math.ceil` for `num_blocks` or truncate both arrays to `min(len(signal), len(reference))`.

### 16. test_op_sqr_polyblep_symmetry: Crashes on numpy ≥2.0 (removed `np.trapz`)

**Test:** `test_op_sqr_polyblep_symmetry.py` line 53
**Severity:** Low (test bug)
**Observed:** `AttributeError: module 'numpy' has no attribute 'trapz'`. `np.trapz` was deprecated in numpy 1.25 and removed in numpy 2.0.
**Fix:** Replace `np.trapz(...)` with `np.trapezoid(...)` (numpy ≥2.0) or `from scipy.integrate import trapezoid`.

---

## Won't Fix / By Design

*(Move items here if investigation shows the behavior is intentional)*

---

## Resolved

*(Move items here once fixed, with commit reference)*

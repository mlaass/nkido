"""
OSC_PHASOR Test
===============
Tests the raw phase output oscillator (0 to 1 ramp, no anti-aliasing).

Implementation: cedar/include/cedar/opcodes/oscillators.hpp:330-359
- Output: raw phase value [0, 1)
- Phase increment: freq / sample_rate
- No anti-aliasing (discontinuity at wrap is intentional)
- Inputs: freq (in0), phase_offset (in1, optional), trigger (in2, optional)
- Uses make_unary (inputs 1,2 use get_input_or_zero)
"""

import os
import numpy as np
import scipy.io.wavfile
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_phasor")


def test_range():
    """
    Test PHASOR output range.

    Expected behavior:
    - All output values in [0, 1)
    - Never negative, never >= 1.0
    """
    print("Test: Output Range [0, 1)")
    host = CedarTestHost()
    freq = 440.0
    freq_buf = host.set_param("freq", freq)

    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_PHASOR, 0, freq_buf, cedar.hash("phasor_range"))
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )

    n_samples = int(2.0 * host.sr)
    output = host.process(np.zeros(n_samples, dtype=np.float32))

    min_val = np.min(output)
    max_val = np.max(output)

    range_ok = min_val >= 0.0 and max_val < 1.0
    if range_ok:
        print(f"  ✓ PASS: Range [{min_val:.6f}, {max_val:.6f}] within [0, 1)")
    else:
        print(f"  ✗ FAIL: Range [{min_val:.6f}, {max_val:.6f}] outside [0, 1)")

    # Save WAV (scaled to [-1, 1] for listening)
    wav_path = os.path.join(OUT, "phasor_440hz.wav")
    scaled = output[:48000] * 2.0 - 1.0  # Map [0,1] to [-1,1]
    scipy.io.wavfile.write(wav_path, host.sr, scaled.astype(np.float32))
    print(f"  Saved {wav_path} - Listen for ascending ramp (buzz)")

    return range_ok


def test_linearity():
    """
    Test PHASOR linearity within each cycle.

    Expected behavior:
    - Within a single cycle, phase increases linearly
    - R² of linear fit > 0.9999
    """
    print("Test: Linearity (R² > 0.9999)")
    host = CedarTestHost()
    freq = 100.0  # Low freq for many samples per cycle
    freq_buf = host.set_param("freq", freq)

    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_PHASOR, 0, freq_buf, cedar.hash("phasor_lin"))
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )

    n_samples = int(0.05 * host.sr)  # 50ms = 5 cycles at 100Hz
    output = host.process(np.zeros(n_samples, dtype=np.float32))

    # Find a complete cycle (skip first cycle)
    samples_per_cycle = int(host.sr / freq)
    # Find the first wrap after the initial block
    wrap_indices = []
    for i in range(128, len(output) - 1):
        if output[i + 1] < output[i] - 0.5:  # Phase wrap
            wrap_indices.append(i)

    if len(wrap_indices) >= 2:
        # Extract one full cycle
        start = wrap_indices[0] + 1
        end = wrap_indices[1] + 1
        cycle = output[start:end]

        # Linear fit
        x = np.arange(len(cycle))
        coeffs = np.polyfit(x, cycle, 1)
        predicted = np.polyval(coeffs, x)
        ss_res = np.sum((cycle - predicted) ** 2)
        ss_tot = np.sum((cycle - np.mean(cycle)) ** 2)
        r_squared = 1.0 - ss_res / ss_tot

        if r_squared > 0.9999:
            print(f"  ✓ PASS: R² = {r_squared:.8f}")
        else:
            print(f"  ✗ FAIL: R² = {r_squared:.8f} (expected > 0.9999)")

        return r_squared > 0.9999
    else:
        print(f"  ✗ FAIL: Not enough phase wraps found ({len(wrap_indices)})")
        return False


def test_frequency_accuracy():
    """
    Test PHASOR frequency accuracy via wrap counting.

    Expected behavior:
    - Number of phase wraps matches expected frequency within 0.1%
    """
    print("Test: Frequency Accuracy (wrap count)")
    test_freqs = [110.0, 440.0, 1000.0, 4000.0]
    all_pass = True

    for freq in test_freqs:
        host = CedarTestHost()
        freq_buf = host.set_param("freq", freq)

        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OSC_PHASOR, 0, freq_buf, cedar.hash(f"phasor_{freq}"))
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
        )

        duration = 1.0
        n_samples = int(duration * host.sr)
        output = host.process(np.zeros(n_samples, dtype=np.float32))

        # Find wrap positions (phase drops by > 0.5)
        wrap_positions = []
        for i in range(1, len(output)):
            if output[i] < output[i - 1] - 0.5:
                # Interpolate exact wrap position
                wrap_positions.append(i)

        # Measure frequency from wrap-to-wrap period (more accurate than counting)
        if len(wrap_positions) >= 3:
            periods = np.diff(wrap_positions)
            avg_period = np.mean(periods)
            measured_freq = host.sr / avg_period
            error_pct = abs(measured_freq - freq) / freq * 100

            if error_pct < 0.1:
                print(f"  ✓ PASS: {freq:8.0f}Hz → {measured_freq:.2f}Hz from {len(wrap_positions)} wraps (error {error_pct:.4f}%)")
            else:
                print(f"  ✗ FAIL: {freq:8.0f}Hz → {measured_freq:.2f}Hz from {len(wrap_positions)} wraps (error {error_pct:.4f}%)")
                all_pass = False
        else:
            print(f"  ✗ FAIL: {freq:8.0f}Hz → not enough wraps ({len(wrap_positions)})")
            all_pass = False

    if all_pass:
        print("  ✓ PASS: All frequencies accurate")
    return all_pass


def test_phase_increment():
    """
    Test PHASOR phase increment matches freq/sr.

    Expected behavior:
    - Sample-to-sample increment ≈ freq / sample_rate (< 0.01% error)
    """
    print("Test: Phase Increment = freq/sr")
    host = CedarTestHost()
    freq = 440.0
    freq_buf = host.set_param("freq", freq)

    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_PHASOR, 0, freq_buf, cedar.hash("phasor_inc"))
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )

    n_samples = int(0.1 * host.sr)
    output = host.process(np.zeros(n_samples, dtype=np.float32))

    expected_inc = freq / host.sr

    # Measure increments (skip wraps and first block)
    increments = []
    for i in range(129, len(output)):
        diff = output[i] - output[i - 1]
        if diff > 0:  # Only non-wrap samples
            increments.append(diff)

    avg_inc = np.mean(increments)
    error_pct = abs(avg_inc - expected_inc) / expected_inc * 100

    if error_pct < 0.01:
        print(f"  ✓ PASS: Increment {avg_inc:.8f} vs expected {expected_inc:.8f} (error {error_pct:.6f}%)")
    else:
        print(f"  ✗ FAIL: Increment {avg_inc:.8f} vs expected {expected_inc:.8f} (error {error_pct:.6f}%)")

    return error_pct < 0.01


def test_waveshaping():
    """
    Test waveshaping: sin(2π·phasor) should produce a sine wave.

    Expected behavior:
    - Applying sin(2π·x) to phasor output produces a sine at the correct frequency
    - Fundamental frequency matches within 0.1%
    """
    print("Test: Waveshaping sin(2π·phasor)")
    host = CedarTestHost()
    freq = 440.0
    freq_buf = host.set_param("freq", freq)

    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_PHASOR, 0, freq_buf, cedar.hash("phasor_ws"))
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )

    duration = 1.0
    n_samples = int(duration * host.sr)
    output = host.process(np.zeros(n_samples, dtype=np.float32))

    # Apply waveshaping in Python
    shaped = np.sin(2.0 * np.pi * output)

    # Save WAV
    wav_path = os.path.join(OUT, "phasor_to_sine.wav")
    scipy.io.wavfile.write(wav_path, host.sr, shaped.astype(np.float32))
    print(f"  Saved {wav_path} - Listen for clean 440Hz sine")

    # FFT to verify fundamental
    n_fft = len(shaped)
    freqs = np.fft.rfftfreq(n_fft, 1 / host.sr)
    fft_mag = np.abs(np.fft.rfft(shaped))
    fft_db = 20 * np.log10(fft_mag + 1e-10)

    # Find peak
    peak_idx = np.argmax(fft_db)
    peak_freq = freqs[peak_idx]
    peak_db = fft_db[peak_idx]

    # Check harmonics (should be minimal for clean sine)
    h2_idx = np.argmin(np.abs(freqs - 2 * freq))
    h3_idx = np.argmin(np.abs(freqs - 3 * freq))
    h2_db = fft_db[h2_idx]
    h3_db = fft_db[h3_idx]

    thd = h2_db - peak_db  # Should be very negative

    freq_error = abs(peak_freq - freq) / freq * 100

    if freq_error < 0.1 and thd < -40:
        print(f"  ✓ PASS: Peak at {peak_freq:.1f}Hz (error {freq_error:.4f}%), THD H2={thd:.1f}dB")
    else:
        print(f"  ✗ FAIL: Peak at {peak_freq:.1f}Hz (error {freq_error:.4f}%), THD H2={thd:.1f}dB")

    return freq_error < 0.1 and thd < -40


if __name__ == "__main__":
    print("=" * 60)
    print("OSC_PHASOR Experiments")
    print("=" * 60)
    print()

    results = {}
    results["range"] = test_range()
    print()
    results["linearity"] = test_linearity()
    print()
    results["frequency"] = test_frequency_accuracy()
    print()
    results["phase_inc"] = test_phase_increment()
    print()
    results["waveshaping"] = test_waveshaping()

    print()
    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    passed = sum(1 for v in results.values() if v)
    total = len(results)
    for name, result in results.items():
        status = "✓ PASS" if result else "✗ FAIL"
        print(f"  {status}: {name}")
    print(f"\n  {passed}/{total} tests passed")

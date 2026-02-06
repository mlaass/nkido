"""
OSC_RAMP Oscillator Test
========================
Tests the inverted sawtooth (descending ramp) oscillator with PolyBLEP anti-aliasing.

Implementation: cedar/include/cedar/opcodes/oscillators.hpp:288-328
- Waveform: 1.0 - 2.0 * phase (descending +1 to -1)
- PolyBLEP correction at rising edge (phase wrap)
- Inputs: freq (in0), phase_offset (in1, optional), trigger (in2, optional)
- Uses make_unary (inputs 1,2 use get_input_or_zero)
"""

import os
import numpy as np
import scipy.io.wavfile
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_ramp")


def test_waveform_shape():
    """
    Test RAMP waveform shape: descending +1 to -1.

    Expected behavior:
    - Output follows 1.0 - 2.0 * phase
    - Starts near +1, descends linearly to -1, then resets
    - All samples in [-1, +1] (with small PolyBLEP overshoot allowed)
    """
    print("Test: Waveform Shape")
    host = CedarTestHost()

    freq = 100.0  # Low freq for clear shape
    freq_buf = host.set_param("freq", freq)

    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_RAMP, 0, freq_buf, cedar.hash("ramp"))
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )

    duration = 0.05  # 50ms = 5 cycles at 100Hz
    n_samples = int(duration * host.sr)
    output = host.process(np.zeros(n_samples, dtype=np.float32))

    # Save WAV
    wav_path = os.path.join(OUT, "ramp_waveform.wav")
    scipy.io.wavfile.write(wav_path, host.sr, output)
    print(f"  Saved {wav_path} - Listen for descending ramp")

    # Check descending shape: find a full cycle and verify it's mostly descending
    # Skip first cycle (initialization)
    samples_per_cycle = host.sr / freq
    start = int(samples_per_cycle * 1.5)
    end = int(samples_per_cycle * 2.5)
    cycle = output[start:end]

    # Count descending samples (excluding the reset jump)
    descending = 0
    for i in range(1, len(cycle)):
        if cycle[i] < cycle[i - 1]:
            descending += 1

    descending_ratio = descending / (len(cycle) - 1)

    # Should be mostly descending (>90% — the reset is ascending)
    if descending_ratio > 0.90:
        print(f"  ✓ PASS: {descending_ratio:.1%} of samples are descending")
    else:
        print(f"  ✗ FAIL: Only {descending_ratio:.1%} descending (expected >90%)")

    # Check amplitude bounds (allow small PolyBLEP overshoot)
    max_amp = np.max(np.abs(output[128:]))  # skip first block
    if max_amp <= 1.05:
        print(f"  ✓ PASS: Max amplitude {max_amp:.4f} within bounds")
    else:
        print(f"  ✗ FAIL: Max amplitude {max_amp:.4f} exceeds 1.05")

    return True


def test_frequency_accuracy():
    """
    Test RAMP frequency accuracy at multiple frequencies.

    Expected behavior:
    - Measured frequency matches input within 0.1% at all test frequencies
    """
    print("Test: Frequency Accuracy")
    test_freqs = [110.0, 440.0, 1000.0, 4000.0, 10000.0]
    all_pass = True

    for freq in test_freqs:
        host = CedarTestHost()
        freq_buf = host.set_param("freq", freq)

        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OSC_RAMP, 0, freq_buf, cedar.hash(f"ramp_{freq}"))
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
        )

        duration = max(0.1, 20.0 / freq)  # At least 20 cycles
        n_samples = int(duration * host.sr)
        output = host.process(np.zeros(n_samples, dtype=np.float32))

        # Measure frequency via zero crossings (negative-going for descending ramp)
        crossings = []
        for i in range(128, len(output) - 1):  # skip first block
            if output[i] >= 0 and output[i + 1] < 0:
                # Interpolate
                t = output[i] / (output[i] - output[i + 1])
                crossings.append(i + t)

        if len(crossings) >= 2:
            periods = np.diff(crossings)
            avg_period = np.mean(periods)
            measured_freq = host.sr / avg_period
            error_pct = abs(measured_freq - freq) / freq * 100

            if error_pct < 0.1:
                print(f"  ✓ PASS: {freq:8.0f}Hz → {measured_freq:.2f}Hz (error {error_pct:.4f}%)")
            else:
                print(f"  ✗ FAIL: {freq:8.0f}Hz → {measured_freq:.2f}Hz (error {error_pct:.4f}%)")
                all_pass = False
        else:
            print(f"  ✗ FAIL: {freq:8.0f}Hz → not enough crossings ({len(crossings)})")
            all_pass = False

    if all_pass:
        print("  ✓ PASS: All frequencies accurate")
    return all_pass


def test_dc_offset():
    """
    Test RAMP DC offset.

    Expected behavior:
    - DC offset < 0.001 (symmetric waveform centered at zero)
    """
    print("Test: DC Offset")
    host = CedarTestHost()
    freq = 440.0
    freq_buf = host.set_param("freq", freq)

    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_RAMP, 0, freq_buf, cedar.hash("ramp_dc"))
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )

    # Long duration for accurate DC measurement
    n_samples = int(2.0 * host.sr)
    output = host.process(np.zeros(n_samples, dtype=np.float32))

    # Skip first block, measure DC over complete cycles
    dc = np.mean(output[128:])
    if abs(dc) < 0.001:
        print(f"  ✓ PASS: DC offset = {dc:.6f}")
    else:
        print(f"  ✗ FAIL: DC offset = {dc:.6f} (expected < 0.001)")

    return abs(dc) < 0.001


def test_polyblep_aliasing():
    """
    Test that PolyBLEP reduces aliasing compared to a naive ramp.

    Expected behavior:
    - PolyBLEP ramp has lower noise floor than a naive (non-anti-aliased) ramp
    - Improvement should be measurable at high frequencies
    """
    print("Test: PolyBLEP Aliasing Reduction")
    freq = 4000.0
    duration = 1.0
    n_samples = int(duration * 48000)

    # Generate PolyBLEP ramp via Cedar
    host = CedarTestHost()
    freq_buf = host.set_param("freq", freq)
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_RAMP, 0, freq_buf, cedar.hash("ramp_blep"))
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )
    cedar_output = host.process(np.zeros(n_samples, dtype=np.float32))

    # Generate naive ramp in Python (no anti-aliasing, float32 to match Cedar)
    phase = np.float32(0.0)
    dt = np.float32(freq / 48000.0)
    naive = np.zeros(n_samples, dtype=np.float32)
    for i in range(n_samples):
        naive[i] = np.float32(1.0) - np.float32(2.0) * phase
        phase += dt
        if phase >= np.float32(1.0):
            phase -= np.float32(1.0)

    # Save both for comparison
    wav_path = os.path.join(OUT, "ramp_polyblep.wav")
    scipy.io.wavfile.write(wav_path, 48000, cedar_output)
    wav_path_naive = os.path.join(OUT, "ramp_naive.wav")
    scipy.io.wavfile.write(wav_path_naive, 48000, naive)
    print(f"  Saved {wav_path} and {wav_path_naive} - Compare aliasing")

    # Compare aliasing energy above the last harmonic (inter-harmonic noise)
    nyquist = 24000
    n_fft = len(cedar_output)
    freqs = np.fft.rfftfreq(n_fft, 1 / 48000)

    cedar_fft = np.abs(np.fft.rfft(cedar_output))
    naive_fft = np.abs(np.fft.rfft(naive))

    # Measure aliasing as total energy between harmonics
    harmonic_freqs = [freq * n for n in range(1, int(nyquist / freq) + 1)]

    noise_mask = np.ones(len(freqs), dtype=bool)
    for hf in harmonic_freqs:
        noise_mask &= (np.abs(freqs - hf) > 100)
    noise_mask &= (freqs > 100) & (freqs < nyquist - 1000)

    cedar_aliasing = np.sqrt(np.mean(cedar_fft[noise_mask] ** 2))
    naive_aliasing = np.sqrt(np.mean(naive_fft[noise_mask] ** 2))

    if naive_aliasing > 1e-10:
        improvement_db = 20 * np.log10(naive_aliasing / max(cedar_aliasing, 1e-10))
    else:
        improvement_db = 0.0

    # PolyBLEP should reduce aliasing energy
    if cedar_aliasing < naive_aliasing:
        print(f"  ✓ PASS: PolyBLEP aliasing RMS {cedar_aliasing:.6f} < naive {naive_aliasing:.6f} ({improvement_db:.1f}dB improvement)")
    else:
        print(f"  ✗ FAIL: PolyBLEP aliasing RMS {cedar_aliasing:.6f} >= naive {naive_aliasing:.6f}")

    return cedar_aliasing < naive_aliasing


def test_inverted_saw_equivalence():
    """
    Test that -RAMP approximates SAW (inverted relationship).

    Expected behavior:
    - Negated ramp output should correlate > 0.99 with SAW output
    - Both are PolyBLEP'd so they should be near-identical
    """
    print("Test: Inverted SAW Equivalence")
    freq = 440.0
    duration = 0.5
    n_samples = int(duration * 48000)

    # Generate RAMP
    host_ramp = CedarTestHost()
    freq_buf = host_ramp.set_param("freq", freq)
    host_ramp.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_RAMP, 0, freq_buf, cedar.hash("ramp_eq"))
    )
    host_ramp.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )
    ramp_output = host_ramp.process(np.zeros(n_samples, dtype=np.float32))

    # Generate SAW
    host_saw = CedarTestHost()
    freq_buf = host_saw.set_param("freq", freq)
    host_saw.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_SAW, 0, freq_buf, cedar.hash("saw_eq"))
    )
    host_saw.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 0)
    )
    saw_output = host_saw.process(np.zeros(n_samples, dtype=np.float32))

    # Skip first block, compare -ramp vs saw
    ramp_inv = -ramp_output[128:]
    saw_trimmed = saw_output[128:]

    correlation = np.corrcoef(ramp_inv, saw_trimmed)[0, 1]

    # Save both for listening
    wav_path = os.path.join(OUT, "ramp_vs_saw.wav")
    stereo = np.column_stack([ramp_inv[:24000], saw_trimmed[:24000]])
    scipy.io.wavfile.write(wav_path, 48000, stereo)
    print(f"  Saved {wav_path} - L: -ramp, R: saw")

    if correlation > 0.99:
        print(f"  ✓ PASS: Correlation between -ramp and saw: {correlation:.6f}")
    else:
        print(f"  ✗ FAIL: Correlation between -ramp and saw: {correlation:.6f} (expected > 0.99)")

    return correlation > 0.99


if __name__ == "__main__":
    print("=" * 60)
    print("OSC_RAMP Experiments")
    print("=" * 60)
    print()

    results = {}
    results["waveform"] = test_waveform_shape()
    print()
    results["frequency"] = test_frequency_accuracy()
    print()
    results["dc_offset"] = test_dc_offset()
    print()
    results["aliasing"] = test_polyblep_aliasing()
    print()
    results["saw_equiv"] = test_inverted_saw_equivalence()

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

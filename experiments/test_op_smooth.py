"""
Test: DISTORT_SMOOTH (ADAA Tanh Saturation)
============================================
Tests first-order ADAA tanh saturation transfer curve and antialiasing.

Expected behavior:
- Transfer curve should match tanh soft-clipping shape
- Higher drive → more harmonic content
- ADAA should reduce aliasing compared to naive tanh at high frequencies
- Output should be bounded (tanh asymptotes)

If this test fails, check the implementation in cedar/include/cedar/opcodes/distortion.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_linear_ramp, save_wav
from visualize import save_figure

OUT = output_dir("op_smooth")


def test_transfer_curve():
    """
    Test DISTORT_SMOOTH transfer curve matches tanh shape.

    Expected:
    - Soft clipping: output approaches ±1 asymptotically
    - Monotonically increasing
    - Symmetric around origin
    """
    print("Test: DISTORT_SMOOTH Transfer Curve")

    ramp = gen_linear_ramp(4096)

    drive_values = [1.0, 3.0, 5.0, 10.0]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_SMOOTH (ADAA Tanh) Transfer Curves")

    all_pass = True
    for drive, ax in zip(drive_values, axes.flat):
        host = CedarTestHost()

        buf_in = 0
        buf_drive = host.set_param("drive", drive)

        host.load_instruction(cedar.Instruction.make_binary(
            cedar.Opcode.DISTORT_SMOOTH, 1, buf_in, buf_drive,
            cedar.hash("smooth") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Reference: tanh(drive * x) normalized
        ref = np.tanh(drive * ramp)

        # Check monotonicity (ADAA can produce small artifacts at block boundaries)
        diffs = np.diff(output)
        is_monotonic = np.all(diffs >= -1e-6)

        # Check symmetry
        mid = len(output) // 2
        pos_half = output[mid:]
        neg_half = -output[mid::-1][:len(pos_half)]
        symmetry_error = np.max(np.abs(pos_half - neg_half))

        # Check bounded output
        is_bounded = np.all(np.abs(output) <= 1.05)

        status = "PASS" if (is_monotonic and is_bounded and symmetry_error < 0.05) else "FAIL"
        if status == "FAIL":
            all_pass = False

        print(f"  Drive {drive}: monotonic={is_monotonic}, bounded={is_bounded}, "
              f"symmetry_err={symmetry_error:.4f} [{status}]")

        ax.plot(ramp, output, linewidth=2, label='ADAA Output')
        ax.plot(ramp, ref, 'r:', alpha=0.5, linewidth=1, label='tanh(drive*x)')
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label='Linear')
        ax.set_title(f'Drive = {drive}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper left', fontsize=8)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.5, 1.5)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "smooth_transfer_curves.png"))
    print(f"  Saved {os.path.join(OUT, 'smooth_transfer_curves.png')}")

    if all_pass:
        print("  ✓ PASS: All transfer curve checks passed")
    else:
        print("  ✗ FAIL: Some transfer curve checks failed")


def test_drive_sweep_harmonics():
    """
    Test that increasing drive produces more harmonic content.

    Expected:
    - THD increases monotonically with drive
    - Predominantly odd harmonics (symmetric saturation)
    """
    print("Test: DISTORT_SMOOTH Drive Sweep Harmonics")

    sr = 48000
    duration = 1.0
    freq = 440.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.7

    drive_values = [1.0, 2.0, 5.0, 10.0, 20.0]
    thd_values = []

    fig, axes = plt.subplots(len(drive_values), 1, figsize=(14, 3 * len(drive_values)))
    fig.suptitle("DISTORT_SMOOTH Harmonic Content vs Drive")

    for idx, drive in enumerate(drive_values):
        host = CedarTestHost(sr)

        buf_in = 0
        buf_drive = host.set_param("drive", drive)

        host.load_instruction(cedar.Instruction.make_binary(
            cedar.Opcode.DISTORT_SMOOTH, 1, buf_in, buf_drive,
            cedar.hash("smooth_harm") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        # FFT analysis
        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs_fft = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Measure THD
        fund_idx = int(freq * fft_size / sr)
        fund_power = spectrum[fund_idx] ** 2
        harmonic_power = 0
        for h in range(2, 10):
            h_idx = fund_idx * h
            if h_idx < len(spectrum):
                harmonic_power += spectrum[h_idx] ** 2
        thd = np.sqrt(harmonic_power / (fund_power + 1e-20)) * 100
        thd_values.append(thd)

        print(f"  Drive {drive:5.1f}: THD = {thd:.2f}%")

        ax = axes[idx]
        mask = (freqs_fft > 100) & (freqs_fft < 8000)
        ax.plot(freqs_fft[mask], spectrum_db[mask], linewidth=0.5)
        ax.set_title(f'Drive={drive} (THD={thd:.1f}%)')
        ax.set_ylabel('dB')
        ax.set_ylim(-100, 0)
        ax.grid(True, alpha=0.3)

    axes[-1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "smooth_drive_harmonics.png"))
    print(f"  Saved {os.path.join(OUT, 'smooth_drive_harmonics.png')}")

    # THD should increase with drive
    is_monotonic = all(thd_values[i] <= thd_values[i + 1] for i in range(len(thd_values) - 1))
    if is_monotonic:
        print("  ✓ PASS: THD increases monotonically with drive")
    else:
        print("  ✗ FAIL: THD does not increase monotonically with drive")


def test_antialiasing():
    """
    Test ADAA reduces aliasing at high frequencies.

    Expected:
    - Noise floor at high frequencies should be lower than naive tanh
    - Most significant at high input frequencies (5kHz+)
    """
    print("Test: DISTORT_SMOOTH Antialiasing Quality")

    sr = 48000
    duration = 0.5
    test_freq = 5000.0  # High frequency where aliasing is most visible
    drive = 8.0

    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * test_freq * t).astype(np.float32) * 0.8

    # Process through ADAA smooth
    host = CedarTestHost(sr)
    buf_in = 0
    buf_drive = host.set_param("drive", drive)
    host.load_instruction(cedar.Instruction.make_binary(
        cedar.Opcode.DISTORT_SMOOTH, 1, buf_in, buf_drive,
        cedar.hash("smooth_aa") & 0xFFFF
    ))
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))
    adaa_output = host.process(sine_input)

    # Naive tanh for comparison
    naive_output = np.tanh(drive * sine_input)

    # Spectral analysis
    fft_size = 8192
    nyquist = sr / 2

    def get_noise_floor(signal):
        steady = signal[int(0.1 * sr):int(0.1 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spec = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spec_db = 20 * np.log10(spec + 1e-10)
        # Mask out harmonics
        harmonic_mask = np.ones(len(freqs), dtype=bool)
        for h in range(1, 20):
            h_freq = test_freq * h
            if h_freq < nyquist:
                h_idx = int(h_freq * fft_size / sr)
                harmonic_mask[max(0, h_idx - 10):min(len(harmonic_mask), h_idx + 10)] = False
        noise_region = spec_db[harmonic_mask & (freqs > 100) & (freqs < nyquist - 100)]
        return np.median(noise_region) if len(noise_region) > 0 else -120

    adaa_floor = get_noise_floor(adaa_output)
    naive_floor = get_noise_floor(naive_output)

    print(f"  ADAA noise floor: {adaa_floor:.1f} dB")
    print(f"  Naive noise floor: {naive_floor:.1f} dB")
    print(f"  Improvement: {naive_floor - adaa_floor:.1f} dB")

    if adaa_floor < naive_floor:
        print("  ✓ PASS: ADAA has lower noise floor than naive tanh")
    else:
        print("  ⚠ WARN: ADAA did not improve noise floor (may still be acceptable)")

    # Plot comparison
    fig, axes = plt.subplots(2, 1, figsize=(14, 8))

    for signal, label, ax in [(adaa_output, "ADAA Smooth", axes[0]),
                               (naive_output, "Naive tanh", axes[1])]:
        steady = signal[int(0.1 * sr):int(0.1 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spec = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spec_db = 20 * np.log10(spec + 1e-10)

        ax.plot(freqs, spec_db, linewidth=0.5)
        ax.set_title(f'{label} (noise floor: {get_noise_floor(signal):.1f} dB)')
        ax.set_xlim(0, nyquist)
        ax.set_ylim(-100, 0)
        ax.set_ylabel('Magnitude (dB)')
        ax.grid(True, alpha=0.3)

    axes[-1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "smooth_antialiasing.png"))
    print(f"  Saved {os.path.join(OUT, 'smooth_antialiasing.png')}")


def test_wav_output():
    """Save WAV of sine through increasing drive levels for listening."""
    print("Test: DISTORT_SMOOTH WAV Output")

    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.8

    for drive in [2.0, 5.0, 10.0, 20.0]:
        host = CedarTestHost(sr)
        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        host.load_instruction(cedar.Instruction.make_binary(
            cedar.Opcode.DISTORT_SMOOTH, 1, buf_in, buf_drive,
            cedar.hash("smooth_wav") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))
        output = host.process(sine_input)

        wav_path = os.path.join(OUT, f"smooth_drive{drive:.0f}.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for smooth tanh saturation (drive={drive})")


if __name__ == "__main__":
    test_transfer_curve()
    test_drive_sweep_harmonics()
    test_antialiasing()
    test_wav_output()

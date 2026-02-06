"""
Test: DISTORT_TAPE (Tape Saturation + Warmth)
==============================================
Tests tape saturation curve, warmth HF rolloff, and drive scaling.

Expected behavior:
- Soft knee saturation onset at threshold
- Warmth parameter controls high-frequency rolloff
- Output should be bounded regardless of drive
- 2x oversampled for reduced aliasing

If this test fails, check the implementation in cedar/include/cedar/opcodes/distortion.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_linear_ramp, save_wav
from visualize import save_figure

OUT = output_dir("op_tape")


def test_saturation_curve():
    """
    Test DISTORT_TAPE saturation curve shape.

    Expected:
    - Soft knee: gradual onset of clipping near threshold
    - Asymptotic bounding of output
    - Smooth, continuous curve
    """
    print("Test: DISTORT_TAPE Saturation Curve")

    ramp = gen_linear_ramp(4096)
    drive_values = [1.0, 3.0, 5.0, 10.0]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_TAPE Transfer Curves")

    for drive, ax in zip(drive_values, axes.flat):
        host = CedarTestHost()

        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_warmth = host.set_param("warmth", 0.3)
        buf_thresh = host.set_param("soft_thresh", 0.5)
        buf_ws = host.set_param("warmth_scale", 0.7)

        host.load_instruction(cedar.Instruction.make_quinary(
            cedar.Opcode.DISTORT_TAPE, 1, buf_in, buf_drive, buf_warmth, buf_thresh, buf_ws,
            cedar.hash("tape") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Check bounded output
        max_abs = np.max(np.abs(output))
        is_bounded = max_abs < 2.0

        print(f"  Drive {drive}: max|output| = {max_abs:.4f}, bounded={is_bounded}")

        ax.plot(ramp, output, linewidth=2, label=f'Tape (drive={drive})')
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label='Linear')
        ax.set_title(f'Drive = {drive}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper left', fontsize=8)
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.5, 1.5)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "tape_transfer_curves.png"))
    print(f"  Saved {os.path.join(OUT, 'tape_transfer_curves.png')}")


def test_warmth_hf_rolloff():
    """
    Test warmth parameter controls HF rolloff.

    Expected:
    - warmth=0: minimal HF change
    - warmth=1: significant HF rolloff
    - Spectral centroid should decrease with warmth
    """
    print("Test: DISTORT_TAPE Warmth HF Rolloff")

    sr = 48000
    duration = 1.0

    # White noise input for broadband analysis
    np.random.seed(42)
    noise = np.random.uniform(-0.5, 0.5, int(duration * sr)).astype(np.float32)

    warmth_values = [0.0, 0.3, 0.6, 1.0]
    centroids = []

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_TAPE Warmth Effect on Spectrum")

    for warmth, ax in zip(warmth_values, axes.flat):
        host = CedarTestHost(sr)

        buf_in = 0
        buf_drive = host.set_param("drive", 3.0)
        buf_warmth = host.set_param("warmth", warmth)
        buf_thresh = host.set_param("soft_thresh", 0.5)
        buf_ws = host.set_param("warmth_scale", 0.7)

        host.load_instruction(cedar.Instruction.make_quinary(
            cedar.Opcode.DISTORT_TAPE, 1, buf_in, buf_drive, buf_warmth, buf_thresh, buf_ws,
            cedar.hash("tape_warmth") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(noise)

        # Spectral analysis
        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Spectral centroid
        centroid = np.sum(freqs * spectrum) / (np.sum(spectrum) + 1e-10)
        centroids.append(centroid)

        ax.semilogx(freqs[1:], spectrum_db[1:], linewidth=0.5)
        ax.set_title(f'Warmth = {warmth} (centroid: {centroid:.0f} Hz)')
        ax.set_xlim(20, sr / 2)
        ax.set_ylim(-80, 0)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "tape_warmth_spectrum.png"))
    print(f"  Saved {os.path.join(OUT, 'tape_warmth_spectrum.png')}")

    # Centroid should decrease with warmth
    centroid_decreasing = all(centroids[i] >= centroids[i + 1] for i in range(len(centroids) - 1))
    for w, c in zip(warmth_values, centroids):
        print(f"  Warmth {w}: spectral centroid = {c:.0f} Hz")

    if centroid_decreasing:
        print("  ✓ PASS: Spectral centroid decreases with warmth")
    else:
        print("  ✗ FAIL: Spectral centroid does not decrease monotonically with warmth")


def test_wav_output():
    """Save WAV of drum-like transient through tape saturation."""
    print("Test: DISTORT_TAPE WAV Output")

    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr

    # Drum-like transient: sine burst with exponential decay
    transient = np.sin(2 * np.pi * 100 * t).astype(np.float32) * np.exp(-5 * t).astype(np.float32)
    # Repeat every 0.5s
    period = int(0.5 * sr)
    signal = np.zeros(int(duration * sr), dtype=np.float32)
    for i in range(4):
        start = i * period
        end = min(start + len(transient), len(signal))
        signal[start:end] += transient[:end - start] * 0.8

    for drive, warmth in [(3.0, 0.2), (6.0, 0.5), (10.0, 0.8)]:
        host = CedarTestHost(sr)
        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_warmth = host.set_param("warmth", warmth)
        buf_thresh = host.set_param("soft_thresh", 0.5)
        buf_ws = host.set_param("warmth_scale", 0.7)

        host.load_instruction(cedar.Instruction.make_quinary(
            cedar.Opcode.DISTORT_TAPE, 1, buf_in, buf_drive, buf_warmth, buf_thresh, buf_ws,
            cedar.hash("tape_wav") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(signal)
        wav_path = os.path.join(OUT, f"tape_d{drive:.0f}_w{warmth:.1f}.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for tape warmth (drive={drive}, warmth={warmth})")


if __name__ == "__main__":
    test_saturation_curve()
    test_warmth_hf_rolloff()
    test_wav_output()

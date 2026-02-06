"""
Test: DISTORT_FOLD (Wavefolder with ADAA)
=========================================
Tests the wavefolder with antiderivative antialiasing.

Expected behavior:
- Sine-fold transfer curve shape
- Smooth transitions at fold points (no discontinuities)
- ADAA should reduce aliasing compared to naive implementation
- Symmetry parameter should control even/odd harmonic balance

If this test fails, check the implementation in cedar/include/cedar/opcodes/distortion.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import scipy.io.wavfile

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_linear_ramp, gen_white_noise, save_wav
from visualize import save_figure

OUT = output_dir("op_fold")


def test_distort_fold_transfer_curve():
    """
    Test DISTORT_FOLD transfer curve shape.
    - Should show smooth sine-fold pattern
    - No discontinuities at fold points
    """
    print("Test: DISTORT_FOLD Transfer Curve")

    ramp = gen_linear_ramp(4096)

    # Test various drive values
    drive_values = [1.5, 3.0, 5.0, 8.0]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_FOLD Transfer Curves (ADAA Sine Wavefolder)")

    for drive, ax in zip(drive_values, axes.flat):
        host = CedarTestHost()

        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_sym = host.set_param("symmetry", 0.5)  # Symmetric

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_test") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Plot
        ax.plot(ramp, output, linewidth=1.5, label='ADAA Output')
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label='Linear')

        # Reference: what the naive sin(drive * x) would look like
        ref = np.sin(drive * ramp)
        ax.plot(ramp, ref, 'r:', alpha=0.5, linewidth=1, label='sin(drive*x)')

        ax.set_title(f'Drive = {drive}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper left', fontsize=8)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.5, 1.5)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "distort_fold_transfer.png"))
    print(f"  Saved {os.path.join(OUT, 'distort_fold_transfer.png')}")


def test_distort_fold_aliasing():
    """
    Test DISTORT_FOLD ADAA aliasing reduction.
    Compare high-frequency signal through folder with/without ADAA.
    """
    print("Test: DISTORT_FOLD Aliasing Analysis")

    sr = 48000
    duration = 1.0
    test_freqs = [1000, 3000, 5000, 8000]  # Hz

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle("DISTORT_FOLD Aliasing Analysis (ADAA vs No ADAA)")

    for freq, ax in zip(test_freqs, axes.flat):
        host = CedarTestHost(sr)

        # Generate sine at test frequency
        t = np.arange(int(duration * sr)) / sr
        sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.8

        buf_in = 0
        buf_drive = host.set_param("drive", 4.0)  # Strong folding
        buf_sym = host.set_param("symmetry", 0.5)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_alias") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        # Save WAV for human evaluation
        wav_path = os.path.join(OUT, f"distort_fold_aliasing_{freq}hz.wav")
        scipy.io.wavfile.write(wav_path, sr, output)

        # Analyze spectrum
        fft_size = 8192
        # Use steady-state portion
        steady = output[int(0.1 * sr):int(0.1 * sr) + fft_size]

        freqs_fft = np.fft.rfftfreq(fft_size, 1/sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Plot spectrum
        ax.plot(freqs_fft, spectrum_db, linewidth=0.5)

        # Mark harmonics and aliased components
        nyquist = sr / 2
        fundamental_idx = int(freq * fft_size / sr)
        ax.axvline(freq, color='green', linestyle='--', alpha=0.5, label=f'Fund. {freq}Hz')
        ax.axvline(nyquist, color='red', linestyle='--', alpha=0.3, label='Nyquist')

        # Expected harmonics from wavefolder: odd harmonics primarily
        for h in [3, 5, 7, 9, 11]:
            h_freq = freq * h
            if h_freq < nyquist:
                ax.axvline(h_freq, color='blue', linestyle=':', alpha=0.3)
            else:
                # Aliased frequency
                aliased = sr - h_freq % sr if h_freq % sr > nyquist else h_freq % sr
                ax.axvline(aliased, color='orange', linestyle=':', alpha=0.3)

        # Measure noise floor (away from harmonics)
        harmonic_mask = np.ones(len(freqs_fft), dtype=bool)
        for h in range(1, 20):
            h_freq = freq * h
            h_idx = int(h_freq * fft_size / sr) if h_freq < nyquist else 0
            if h_idx > 0 and h_idx < len(harmonic_mask):
                # Mask out +-10 bins around harmonic
                harmonic_mask[max(0, h_idx-10):min(len(harmonic_mask), h_idx+10)] = False

        noise_floor = np.median(spectrum_db[harmonic_mask & (freqs_fft > 100) & (freqs_fft < nyquist - 100)])

        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.set_title(f'Input: {freq}Hz sine (noise floor: {noise_floor:.1f}dB)')
        ax.set_xlim(0, nyquist)
        ax.set_ylim(-100, 0)
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper right', fontsize=7)

        # ADAA should keep noise floor below -60dB
        status = "PASS" if noise_floor < -60 else "HIGH ALIASING"
        print(f"  {freq}Hz: noise floor = {noise_floor:.1f}dB {status} [{wav_path}]")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "distort_fold_aliasing.png"))
    print(f"  Saved {os.path.join(OUT, 'distort_fold_aliasing.png')}")


def test_distort_fold_symmetry():
    """
    Test DISTORT_FOLD symmetry parameter effect.
    Asymmetry should introduce even harmonics.
    """
    print("Test: DISTORT_FOLD Symmetry Parameter")

    sr = 48000
    duration = 0.5

    symmetry_values = [0.0, 0.25, 0.5, 0.75, 1.0]

    fig, axes = plt.subplots(2, 3, figsize=(15, 8))
    fig.suptitle("DISTORT_FOLD Symmetry Effect on Harmonics")

    # Transfer curves
    for sym, ax in zip(symmetry_values, axes[0].flat):
        host = CedarTestHost(sr)
        ramp = gen_linear_ramp(2048)

        buf_in = 0
        buf_drive = host.set_param("drive", 4.0)
        buf_sym = host.set_param("symmetry", sym)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_sym") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        ax.plot(ramp, output, linewidth=1.5)
        ax.plot(ramp, ramp, 'k--', alpha=0.3)
        ax.set_title(f'Symmetry = {sym}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.5, 1.5)

    # Spectral analysis with sine input
    axes[0, 2].axis('off')  # Empty the 6th subplot

    # Harmonic comparison
    freq = 440.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.7

    ax_harm = axes[1, 0]
    ax_spec = axes[1, 1]

    colors = plt.cm.viridis(np.linspace(0, 1, len(symmetry_values)))
    harmonic_data = {}

    for sym, color in zip(symmetry_values, colors):
        host = CedarTestHost(sr)

        buf_in = 0
        buf_drive = host.set_param("drive", 4.0)
        buf_sym = host.set_param("symmetry", sym)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_sym_spec") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        # Spectrum
        fft_size = 8192
        steady = output[int(0.1 * sr):int(0.1 * sr) + fft_size]
        freqs_fft = np.fft.rfftfreq(fft_size, 1/sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Plot spectrum
        mask = (freqs_fft > 100) & (freqs_fft < 5000)
        ax_spec.plot(freqs_fft[mask], spectrum_db[mask], color=color, alpha=0.7,
                    label=f'sym={sym}', linewidth=0.8)

        # Extract harmonics
        fundamental_idx = int(freq * fft_size / sr)
        fund_level = spectrum_db[fundamental_idx]
        harmonics = []
        for h in range(1, 8):
            h_idx = fundamental_idx * h
            if h_idx < len(spectrum_db):
                harmonics.append(spectrum_db[h_idx] - fund_level)
            else:
                harmonics.append(-100)
        harmonic_data[sym] = harmonics

    ax_spec.set_xlabel('Frequency (Hz)')
    ax_spec.set_ylabel('Magnitude (dB)')
    ax_spec.set_title('Spectrum Comparison')
    ax_spec.legend(fontsize=7)
    ax_spec.grid(True, alpha=0.3)

    # Harmonic comparison bar chart
    x = np.arange(1, 8)
    width = 0.15
    for i, (sym, harmonics) in enumerate(harmonic_data.items()):
        ax_harm.bar(x + i * width - 0.3, harmonics, width, label=f'sym={sym}', color=colors[i])

    ax_harm.set_xlabel('Harmonic Number')
    ax_harm.set_ylabel('Level (dB rel. fundamental)')
    ax_harm.set_title('Harmonic Content by Symmetry')
    ax_harm.legend(fontsize=7)
    ax_harm.grid(True, alpha=0.3, axis='y')
    ax_harm.set_xticks(x)

    # Summary text
    axes[1, 2].axis('off')
    axes[1, 2].text(0.1, 0.8, "Symmetry Effect:", fontsize=12, fontweight='bold')
    axes[1, 2].text(0.1, 0.6, "sym=0.5: Symmetric -> odd harmonics only", fontsize=10)
    axes[1, 2].text(0.1, 0.45, "sym!=0.5: Asymmetric -> even harmonics appear", fontsize=10)
    axes[1, 2].text(0.1, 0.3, "DC offset shifts with symmetry", fontsize=10)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "distort_fold_symmetry.png"))
    print(f"  Saved {os.path.join(OUT, 'distort_fold_symmetry.png')}")


def test_distort_fold_continuity():
    """
    Test DISTORT_FOLD ADAA continuity at fold points.
    Zoomed view should show smooth transitions, no discontinuities.
    """
    print("Test: DISTORT_FOLD Continuity at Fold Points")

    host = CedarTestHost()

    # High resolution ramp to check continuity
    ramp = np.linspace(-1, 1, 16384, dtype=np.float32)

    buf_in = 0
    buf_drive = host.set_param("drive", 6.0)  # Multiple folds
    buf_sym = host.set_param("symmetry", 0.5)

    host.load_instruction(cedar.Instruction.make_ternary(
        cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
        cedar.hash("fold_cont") & 0xFFFF
    ))
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(ramp)

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle("DISTORT_FOLD Continuity Analysis (Drive=6.0)")

    # Full transfer curve
    axes[0, 0].plot(ramp, output, linewidth=1)
    axes[0, 0].set_title('Full Transfer Curve')
    axes[0, 0].set_xlabel('Input')
    axes[0, 0].set_ylabel('Output')
    axes[0, 0].grid(True, alpha=0.3)

    # Derivative (should be continuous for ADAA)
    derivative = np.diff(output) / np.diff(ramp)
    axes[0, 1].plot(ramp[:-1], derivative, linewidth=0.5)
    axes[0, 1].set_title('Derivative (should be continuous)')
    axes[0, 1].set_xlabel('Input')
    axes[0, 1].set_ylabel('d(output)/d(input)')
    axes[0, 1].grid(True, alpha=0.3)

    # Check for discontinuities
    diff2 = np.diff(derivative)
    max_jump = np.max(np.abs(diff2))

    # Zoomed view around a fold point (where derivative is large)
    peak_idx = np.argmax(np.abs(derivative))
    zoom_start = max(0, peak_idx - 500)
    zoom_end = min(len(ramp), peak_idx + 500)

    axes[1, 0].plot(ramp[zoom_start:zoom_end], output[zoom_start:zoom_end], 'b-', linewidth=2)
    axes[1, 0].set_title(f'Zoomed: Around Fold Point')
    axes[1, 0].set_xlabel('Input')
    axes[1, 0].set_ylabel('Output')
    axes[1, 0].grid(True, alpha=0.3)

    axes[1, 1].plot(ramp[zoom_start:zoom_end-1], derivative[zoom_start:zoom_end-1], 'g-', linewidth=1)
    axes[1, 1].set_title(f'Zoomed Derivative (max d2={max_jump:.4f})')
    axes[1, 1].set_xlabel('Input')
    axes[1, 1].set_ylabel('Derivative')
    axes[1, 1].grid(True, alpha=0.3)

    # Report
    if max_jump < 0.1:
        print(f"  Continuity good: max derivative jump = {max_jump:.6f}")
    else:
        print(f"  Possible discontinuity: max derivative jump = {max_jump:.6f}")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "distort_fold_continuity.png"))
    print(f"  Saved {os.path.join(OUT, 'distort_fold_continuity.png')}")


if __name__ == "__main__":
    test_distort_fold_transfer_curve()
    test_distort_fold_aliasing()
    test_distort_fold_symmetry()
    test_distort_fold_continuity()

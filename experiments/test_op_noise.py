"""
NOISE Opcode Quality Test (Cedar Engine)
=========================================
Tests white noise generator for distribution properties and spectral flatness.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder
from visualize import save_figure

OUT = output_dir("op_noise")


# =============================================================================
# NOISE Test - Distribution and Spectral Flatness
# =============================================================================

def test_noise_distribution():
    """
    Test white noise generator for:
    - Uniform distribution in [-1, 1]
    - Mean near 0
    - Standard deviation near 0.577 (uniform distribution std)
    - Spectral flatness (no peaks)
    """
    print("Test 1: NOISE Distribution and Spectral Flatness")
    print("=" * 60)

    sr = 48000
    duration = 10.0  # 10 seconds of noise for good statistics
    num_samples = int(duration * sr)

    host = CedarTestHost(sr)

    # NOISE opcode: out = noise generator
    # Most noise opcodes are nullary (no inputs)
    buf_out = 1
    host.load_instruction(
        cedar.Instruction.make_nullary(cedar.Opcode.NOISE, buf_out, cedar.hash("noise") & 0xFFFF)
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    # Generate silence as input (noise is self-generating)
    silence = np.zeros(num_samples, dtype=np.float32)
    noise_output = host.process(silence)

    results = {'sample_rate': sr, 'duration': duration, 'tests': []}

    # Statistics analysis
    mean = float(np.mean(noise_output))
    std = float(np.std(noise_output))
    min_val = float(np.min(noise_output))
    max_val = float(np.max(noise_output))

    # For uniform distribution in [-1, 1]: mean=0, std=1/sqrt(3)≈0.577
    expected_std = 1.0 / np.sqrt(3)
    std_error = abs(std - expected_std) / expected_std * 100

    results['tests'].append({
        'name': 'Distribution statistics',
        'mean': mean,
        'std': std,
        'expected_std': expected_std,
        'std_error_pct': std_error,
        'min': min_val,
        'max': max_val
    })

    print(f"\n  Distribution Statistics:")
    print(f"    Mean:     {mean:.6f} (expected: ~0)")
    print(f"    Std Dev:  {std:.4f} (expected: {expected_std:.4f}, error: {std_error:.1f}%)")
    print(f"    Range:    [{min_val:.4f}, {max_val:.4f}] (expected: [-1, 1])")

    mean_ok = abs(mean) < 0.01  # Mean within 1% of range
    std_ok = std_error < 10  # Std within 10%
    range_ok = min_val >= -1.05 and max_val <= 1.05  # Allow 5% tolerance

    print(f"    Mean OK:  {'PASS' if mean_ok else 'FAIL'}")
    print(f"    Std OK:   {'PASS' if std_ok else 'FAIL'}")
    print(f"    Range OK: {'PASS' if range_ok else 'FAIL'}")

    # Spectral analysis - should be flat
    print("\n  Spectral Analysis:")
    # Use 1 second of noise for FFT
    fft_samples = sr
    fft_data = noise_output[:fft_samples]
    freqs = np.fft.rfftfreq(fft_samples, 1/sr)
    spectrum = np.abs(np.fft.rfft(fft_data))
    spectrum_db = 20 * np.log10(spectrum + 1e-10)

    # Check flatness by looking at variance across frequency bands
    # Divide spectrum into octave bands and check uniformity
    bands = [(20, 100), (100, 500), (500, 2000), (2000, 8000), (8000, 20000)]
    band_levels = []

    for low, high in bands:
        mask = (freqs >= low) & (freqs < high)
        if np.any(mask):
            band_avg = np.mean(spectrum_db[mask])
            band_levels.append(band_avg)

    band_variance = np.var(band_levels)
    spectral_flatness_ok = band_variance < 20  # dB variance

    results['tests'].append({
        'name': 'Spectral flatness',
        'band_levels_db': band_levels,
        'band_variance_db': float(band_variance),
        'passed': spectral_flatness_ok
    })

    print(f"    Band levels (dB): {[f'{l:.1f}' for l in band_levels]}")
    print(f"    Band variance: {band_variance:.2f} dB")
    print(f"    Flatness OK: {'PASS' if spectral_flatness_ok else 'FAIL'}")

    # Create visualization
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Histogram
    ax1 = axes[0, 0]
    ax1.hist(noise_output, bins=100, density=True, alpha=0.7, edgecolor='black')
    ax1.axvline(0, color='red', linestyle='--', alpha=0.5)
    # Theoretical uniform distribution
    x_uniform = np.linspace(-1, 1, 100)
    ax1.plot(x_uniform, np.ones_like(x_uniform) * 0.5, 'g-', linewidth=2, label='Uniform PDF')
    ax1.set_xlabel('Value')
    ax1.set_ylabel('Density')
    ax1.set_title(f'Noise Distribution (mean={mean:.4f}, std={std:.4f})')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Spectrum
    ax2 = axes[0, 1]
    ax2.semilogx(freqs[1:], spectrum_db[1:], linewidth=0.5, alpha=0.7)
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude (dB)')
    ax2.set_title('Noise Spectrum')
    ax2.set_xlim(20, sr/2)
    ax2.grid(True, which='both', alpha=0.3)

    # Waveform snippet
    ax3 = axes[1, 0]
    time_ms = np.arange(2000) / sr * 1000
    ax3.plot(time_ms, noise_output[:2000], linewidth=0.5)
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Amplitude')
    ax3.set_title('Noise Waveform (first 2000 samples)')
    ax3.grid(True, alpha=0.3)

    # Autocorrelation (should be near-delta for white noise)
    ax4 = axes[1, 1]
    autocorr_samples = 1000
    autocorr = np.correlate(noise_output[:autocorr_samples],
                            noise_output[:autocorr_samples], mode='full')
    autocorr = autocorr[autocorr_samples-1:autocorr_samples+100]
    autocorr = autocorr / autocorr[0]  # Normalize
    ax4.plot(np.arange(len(autocorr)), autocorr, linewidth=1)
    ax4.set_xlabel('Lag (samples)')
    ax4.set_ylabel('Autocorrelation')
    ax4.set_title('Autocorrelation (should be ~0 except at lag 0)')
    ax4.axhline(0, color='gray', linestyle='--', alpha=0.5)
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'distribution.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'distribution.png')}")

    with open(os.path.join(OUT, 'distribution.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'distribution.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar NOISE Opcode Quality Test")
    print("=" * 60)
    print()

    test_noise_distribution()

    print()
    print("=" * 60)
    print("NOISE test complete.")

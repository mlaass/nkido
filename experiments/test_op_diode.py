"""
Test: Diode Ladder Filter (FILTER_DIODE)
==========================================
Tests the diode ladder filter (TB-303 style) frequency response,
self-oscillation behavior, and character comparison with Moog ladder.

Expected behavior:
- Asymmetric clipping character from diode nonlinearity
- Self-oscillation at resonance ~3.5+ with proper VT/FB_GAIN tuning
- Oscillation frequency should match cutoff frequency within 5%
- Different character from Moog ladder (less bass loss, different harmonic content)

If this test fails, check the implementation in cedar/include/cedar/opcodes/filters.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import scipy.io.wavfile
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from filter_helpers import analyze_filter, get_bode_data, get_impulse
from visualize import save_figure

OUT = output_dir("op_diode")


def analyze_diode_filter(cutoff, res, vt=0.026, fb_gain=10.0, filter_name="Diode"):
    """
    Runs an impulse through FILTER_DIODE with tunable vt/fb_gain parameters.
    """
    sr = 48000
    host = CedarTestHost(sr)

    buf_in = 0
    buf_freq = host.set_param("cutoff", cutoff)
    buf_res = host.set_param("res", res)
    buf_vt = host.set_param("vt", vt)
    buf_fb_gain = host.set_param("fb_gain", fb_gain)
    buf_out = 1

    state_id = cedar.hash(f"{filter_name}_state") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_quinary(
            cedar.Opcode.FILTER_DIODE, buf_out, buf_in,
            buf_freq, buf_res, buf_vt, buf_fb_gain, state_id
        )
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    impulse = get_impulse(0.1, sr)
    response = host.process(impulse)
    freqs, mag_db = get_bode_data(response, sr)
    return freqs, mag_db


def test_diode_frequency_response():
    """Test FILTER_DIODE frequency response and resonance sweep."""
    print("Test: Diode Ladder Filter Frequency Response")

    cutoff = 1000.0
    resonance_values = [0.0, 1.0, 2.0, 3.0, 3.5]  # 3.5+ = self-oscillation

    plt.figure(figsize=(12, 6))

    for res in resonance_values:
        freqs, mag = analyze_diode_filter(cutoff, res, vt=0.026, fb_gain=10.0)
        plt.semilogx(freqs, mag, label=f'Resonance {res}')

    plt.title(f'Diode Ladder Filter (Fc={cutoff}Hz)')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 30)
    plt.xlim(20, 20000)

    # Mark cutoff frequency
    plt.axvline(cutoff, color='gray', linestyle='--', alpha=0.5, label='Cutoff')

    save_figure(plt.gcf(), os.path.join(OUT, "diode_response.png"))
    print(f"  Saved {os.path.join(OUT, 'diode_response.png')}")


def test_diode_self_oscillation():
    """
    Test FILTER_DIODE self-oscillation at high resonance with different VT/FB_GAIN configs.

    Expected behavior (per implementation comments):
    - Self-oscillation should occur at resonance ~3.5+ with proper VT/FB_GAIN tuning
    - Oscillation frequency should match cutoff frequency within 5%

    Configurations tested:
    - Original (VT=0.026, FB_GAIN=1.0): No oscillation expected
    - A (VT=0.026, FB_GAIN=10.0): Self-oscillation expected
    - B (VT=0.05, FB_GAIN=5.0): Middle ground
    - C (VT=0.1, FB_GAIN=2.5): Softer character

    If this test fails, check the implementation in cedar/include/cedar/opcodes/filters.hpp
    """
    print("Test: Diode Ladder Self-Oscillation (VT/FB_GAIN comparison)")

    sr = 48000
    duration = 0.5  # 500ms
    cutoff = 1000.0
    resonance = 3.8  # Should self-oscillate per implementation comments

    # Test configurations: (name, vt, fb_gain, expect_oscillation)
    configs = [
        ("Original", 0.026, 1.0, False),   # No compensation - should NOT oscillate
        ("A_fb10", 0.026, 10.0, True),     # High feedback gain - should oscillate
        ("B_mid", 0.05, 5.0, True),        # Middle ground
        ("C_soft", 0.1, 2.5, True),        # Softer character
    ]

    fig, axes = plt.subplots(len(configs), 2, figsize=(14, 12))
    fig.suptitle(f'Diode Ladder Self-Oscillation: VT/FB_GAIN Comparison (Cutoff={cutoff}Hz, Res={resonance})')

    results = []

    for idx, (name, vt, fb_gain, expect_osc) in enumerate(configs):
        host = CedarTestHost(sr)

        buf_in = 0
        buf_freq = host.set_param("cutoff", cutoff)
        buf_res = host.set_param("res", resonance)
        buf_vt = host.set_param("vt", vt)
        buf_fb_gain = host.set_param("fb_gain", fb_gain)
        buf_out = 1

        state_id = cedar.hash(f"diode_osc_{name}") & 0xFFFF
        host.load_instruction(
            cedar.Instruction.make_quinary(
                cedar.Opcode.FILTER_DIODE, buf_out, buf_in,
                buf_freq, buf_res, buf_vt, buf_fb_gain, state_id
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Feed short noise burst to excite oscillation, then silence
        signal = np.zeros(int(duration * sr), dtype=np.float32)
        signal[:100] = np.random.uniform(-0.5, 0.5, 100).astype(np.float32)
        output = host.process(signal)

        # Save WAV for human evaluation
        wav_path = os.path.join(OUT, f"diode_selfoscillation_{name}.wav")
        scipy.io.wavfile.write(wav_path, sr, output)

        # Analyze steady-state (after 100ms)
        steady = output[int(0.1 * sr):]

        # Time domain plot
        time_ms = np.arange(len(steady[:2000])) / sr * 1000
        axes[idx, 0].plot(time_ms, steady[:2000])
        axes[idx, 0].set_xlabel('Time (ms)')
        axes[idx, 0].set_ylabel('Amplitude')
        axes[idx, 0].set_title(f'{name}: VT={vt}, FB_GAIN={fb_gain}')
        axes[idx, 0].grid(True, alpha=0.3)

        # Check if oscillating
        max_amp = np.max(np.abs(steady))
        is_oscillating = max_amp > 0.01

        if is_oscillating:
            fft_size = 8192
            freqs = np.fft.rfftfreq(fft_size, 1/sr)
            spectrum = np.abs(np.fft.rfft(steady[:fft_size]))
            spectrum_db = 20 * np.log10(spectrum + 1e-10)

            peak_idx = np.argmax(spectrum)
            peak_freq = freqs[peak_idx]
            freq_error = abs(peak_freq - cutoff) / cutoff * 100

            axes[idx, 1].plot(freqs, spectrum_db)
            axes[idx, 1].axvline(cutoff, color='red', linestyle='--', alpha=0.7, label=f'Expected {cutoff}Hz')
            axes[idx, 1].axvline(peak_freq, color='green', linestyle=':', alpha=0.7, label=f'Actual {peak_freq:.0f}Hz')
            axes[idx, 1].set_xlabel('Frequency (Hz)')
            axes[idx, 1].set_ylabel('Magnitude (dB)')
            axes[idx, 1].set_title(f'Spectrum (error: {freq_error:.1f}%)')
            axes[idx, 1].set_xlim(0, cutoff * 3)
            axes[idx, 1].legend()
            axes[idx, 1].grid(True, alpha=0.3)

            # Check if result matches expectation
            if expect_osc:
                status = "✓ PASS" if freq_error < 5 else "⚠ FREQ ERROR"
            else:
                status = "⚠ UNEXPECTED OSCILLATION"
            print(f"  {name} (VT={vt}, FB={fb_gain}): oscillates at {peak_freq:.1f}Hz (error: {freq_error:.1f}%) {status}")
            results.append((name, True, freq_error < 5 if expect_osc else False))
        else:
            axes[idx, 1].text(0.5, 0.5, 'NO OSCILLATION',
                            ha='center', va='center', transform=axes[idx, 1].transAxes,
                            fontsize=12, color='orange' if not expect_osc else 'red')
            axes[idx, 1].set_xlim(0, cutoff * 3)
            axes[idx, 1].set_xlabel('Frequency (Hz)')
            axes[idx, 1].set_ylabel('Magnitude (dB)')
            axes[idx, 1].grid(True, alpha=0.3)

            if expect_osc:
                status = "✗ FAIL - No oscillation"
                results.append((name, False, False))
            else:
                status = "✓ PASS - No oscillation (as expected)"
                results.append((name, False, True))
            print(f"  {name} (VT={vt}, FB={fb_gain}): {status} (max amp: {max_amp:.6f})")

        print(f"    Saved {wav_path} - Listen for sine-like tone at {cutoff}Hz")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "diode_selfoscillation.png"))
    print(f"  Saved {os.path.join(OUT, 'diode_selfoscillation.png')}")

    # Summary
    passed = sum(1 for _, _, ok in results if ok)
    total = len(results)
    print(f"\n  Summary: {passed}/{total} configurations behaved as expected")
    if passed < total:
        print("  ⚠ WARNING: Some configurations didn't match expectations. Review results.")


def test_diode_vs_moog():
    """Compare FILTER_DIODE and FILTER_MOOG character."""
    print("Test: Diode vs Moog Character Comparison")

    cutoff = 1000.0
    resonance = 3.0  # High but not self-oscillating

    plt.figure(figsize=(12, 6))

    # Use default VT/FB_GAIN for diode filter
    freqs_diode, mag_diode = analyze_diode_filter(cutoff, resonance, vt=0.026, fb_gain=10.0)
    freqs_moog, mag_moog = analyze_filter(cedar.Opcode.FILTER_MOOG, cutoff, resonance, "Moog")

    plt.semilogx(freqs_diode, mag_diode, label='Diode Ladder (TB-303)', linewidth=2)
    plt.semilogx(freqs_moog, mag_moog, label='Moog Ladder', linewidth=2, linestyle='--')

    plt.title(f'Diode vs Moog Ladder Comparison (Fc={cutoff}Hz, Res={resonance})')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 30)
    plt.xlim(20, 20000)
    plt.axvline(cutoff, color='gray', linestyle='--', alpha=0.5)

    save_figure(plt.gcf(), os.path.join(OUT, "diode_vs_moog.png"))
    print(f"  Saved {os.path.join(OUT, 'diode_vs_moog.png')}")


if __name__ == "__main__":
    print("=== Diode Ladder (FILTER_DIODE) Tests ===\n")
    test_diode_frequency_response()
    test_diode_self_oscillation()
    test_diode_vs_moog()

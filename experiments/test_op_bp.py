"""
Test: SVF Bandpass Filter (FILTER_SVF_BP)
==========================================
Tests the state-variable filter bandpass mode at various cutoff and resonance settings.

Expected behavior:
- Bandpass response: peak at cutoff frequency, attenuates both below and above
- Center frequency should match the specified cutoff
- Resonance (Q) controls bandwidth - higher Q = narrower bandwidth
- Gain at center frequency should remain consistent across Q values

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

OUT = output_dir("op_bp")


def test_svf_bp_cutoff_sweep():
    """Test SVF BP at multiple center frequencies with moderate resonance."""
    print("Test: SVF Bandpass - Center Frequency Sweep")

    q = 2.0  # Mild resonance
    cutoffs = [500, 1000, 2000, 4000]

    plt.figure(figsize=(12, 6))

    for cutoff in cutoffs:
        freqs, mag = analyze_filter(cedar.Opcode.FILTER_SVF_BP, cutoff, q, f"BP_{cutoff}")
        plt.semilogx(freqs, mag, label=f'Fc={cutoff}Hz')

    plt.title(f'SVF Bandpass - Center Frequency Sweep (Q={q})')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 10)
    plt.xlim(20, 20000)

    save_figure(plt.gcf(), os.path.join(OUT, "svf_bp_cutoff_sweep.png"))
    print(f"  Saved {os.path.join(OUT, 'svf_bp_cutoff_sweep.png')}")


def test_svf_bp_resonance_sweep():
    """Test SVF BP with multiple resonance values at fixed center frequency."""
    print("Test: SVF Bandpass - Resonance Sweep")

    cutoff = 1000.0
    resonance_values = [0.5, 1.0, 2.0, 4.0, 8.0]

    plt.figure(figsize=(12, 6))

    for q in resonance_values:
        freqs, mag = analyze_filter(cedar.Opcode.FILTER_SVF_BP, cutoff, q, f"BP_Q{q}")
        plt.semilogx(freqs, mag, label=f'Q={q}')

    plt.title(f'SVF Bandpass - Resonance Sweep (Fc={cutoff}Hz)')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 20)
    plt.xlim(20, 20000)
    plt.axvline(cutoff, color='gray', linestyle='--', alpha=0.5, label='Center')

    save_figure(plt.gcf(), os.path.join(OUT, "svf_bp_resonance_sweep.png"))
    print(f"  Saved {os.path.join(OUT, 'svf_bp_resonance_sweep.png')}")


def test_svf_bp_filtered_noise():
    """Filter white noise through SVF BP and save WAV files for listening."""
    print("Test: SVF Bandpass - Filtered Noise")

    sr = 48000
    duration = 2.0
    cutoffs = [500, 1000, 2000, 4000]
    q = 2.0

    for cutoff in cutoffs:
        host = CedarTestHost(sr)

        noise = np.random.uniform(-0.3, 0.3, int(duration * sr)).astype(np.float32)

        buf_in = 0
        buf_freq = host.set_param("cutoff", float(cutoff))
        buf_res = host.set_param("res", q)
        buf_out = 1

        state_id = cedar.hash(f"bp_noise_{cutoff}") & 0xFFFF
        host.load_instruction(
            cedar.Instruction.make_ternary(
                cedar.Opcode.FILTER_SVF_BP, buf_out, buf_in, buf_freq, buf_res, state_id
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        output = host.process(noise)

        wav_path = os.path.join(OUT, f"svf_bp_noise_{cutoff}Hz.wav")
        scipy.io.wavfile.write(wav_path, sr, output)
        print(f"  Saved {wav_path} - Listen for bandpass filtered noise centered at {cutoff}Hz")


if __name__ == "__main__":
    print("=== SVF Bandpass (FILTER_SVF_BP) Tests ===\n")
    test_svf_bp_cutoff_sweep()
    test_svf_bp_resonance_sweep()
    test_svf_bp_filtered_noise()

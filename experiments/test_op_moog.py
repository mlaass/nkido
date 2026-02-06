"""
Test: Moog Ladder Filter (FILTER_MOOG)
=======================================
Tests the Moog ladder filter resonance behavior.

Expected behavior:
- 24 dB/octave lowpass slope
- Resonance creates a peak at the cutoff frequency
- At resonance ~4.0, the filter should approach self-oscillation
- Increasing resonance should reduce passband gain (classic Moog behavior)

If this test fails, check the implementation in cedar/include/cedar/opcodes/filters.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from filter_helpers import analyze_filter, get_bode_data, get_impulse
from visualize import save_figure

OUT = output_dir("op_moog")


def test_moog_resonance():
    """Test Moog ladder filter resonance sweeps."""
    print("Test: Moog Ladder Resonance Sweeps")

    cutoff = 2000.0
    resonance_values = [0.0, 1.0, 2.0, 3.0, 3.8]  # 4.0 is self-oscillation

    plt.figure(figsize=(12, 6))

    for res in resonance_values:
        freqs, mag = analyze_filter(cedar.Opcode.FILTER_MOOG, cutoff, res, "Moog")
        plt.semilogx(freqs, mag, label=f'Resonance {res}')

    plt.title(f'Moog Ladder Resonance (Fc={cutoff}Hz)')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 20)
    plt.xlim(20, 20000)

    save_figure(plt.gcf(), os.path.join(OUT, "moog_resonance.png"))
    print(f"  Saved {os.path.join(OUT, 'moog_resonance.png')}")


if __name__ == "__main__":
    print("=== Moog Ladder (FILTER_MOOG) Tests ===\n")
    test_moog_resonance()

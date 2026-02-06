"""
Test: DISTORT_BITCRUSH (Bitcrusher)
===================================
Tests bit depth reduction and sample rate reduction.

Expected behavior:
- Bit depth reduction should create discrete quantization steps
- N-bit reduction should produce approximately 2^N distinct output levels
- Sample rate reduction should introduce aliasing artifacts
- Lower bit depths should produce more audible staircase distortion

If this test fails, check the implementation in cedar/include/cedar/opcodes/distortion.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_bitcrush")


def test_bitcrush_levels():
    """
    Test bit depth reduction creates discrete steps.
    - Input: slow sine sweep
    - Count distinct output levels for given bit depth
    """
    print("Test: Bitcrush Quantization Levels")

    sr = 48000
    duration = 1.0

    # Test various bit depths
    bit_depths = [8, 4, 3, 2]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for idx, bits in enumerate(bit_depths):
        host = CedarTestHost(sr)

        # Generate slow sine (one cycle over duration)
        t = np.arange(int(duration * sr)) / sr
        sine_input = np.sin(2 * np.pi * 1 * t).astype(np.float32)  # 1 Hz

        buf_in = 0
        buf_bits = host.set_param("bits", float(bits))
        buf_rate = host.set_param("rate", 1.0)  # No sample rate reduction
        buf_out = 1

        # DISTORT_BITCRUSH: out = bitcrush(in, bits, rate_factor)
        inst = cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_BITCRUSH, buf_out, buf_in, buf_bits, buf_rate, cedar.hash("crush") & 0xFFFF
        )
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

        output = host.process(sine_input)

        # Count unique levels (with some tolerance for floating point)
        unique_levels = len(np.unique(np.round(output, 4)))
        expected_levels = 2 ** bits

        print(f"  {bits}-bit: expected {expected_levels} levels, measured {unique_levels}")

        # Plot transfer curve
        ax = axes[idx // 2, idx % 2]
        ax.plot(sine_input, output, 'b.', markersize=0.5, alpha=0.5)
        ax.plot([-1, 1], [-1, 1], 'k--', alpha=0.3, label='Linear')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.set_title(f'{bits}-bit Bitcrush (expected: {expected_levels} levels)')
        ax.set_aspect('equal')
        ax.grid(True, alpha=0.3)
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.1, 1.1)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "bitcrush_levels.png"))
    print(f"  Saved {os.path.join(OUT, 'bitcrush_levels.png')}")

    # Test sample rate reduction
    print("\n  Sample Rate Reduction Test:")
    host2 = CedarTestHost(sr)

    # High frequency sine to show sample rate reduction
    t2 = np.arange(int(0.1 * sr)) / sr
    hf_sine = np.sin(2 * np.pi * 5000 * t2).astype(np.float32)  # 5kHz

    buf_in2 = 0
    buf_bits2 = host2.set_param("bits", 16.0)  # Full bit depth
    buf_rate2 = host2.set_param("rate", 0.1)  # 10% sample rate = 4.8kHz effective
    buf_out2 = 1

    inst2 = cedar.Instruction.make_ternary(
        cedar.Opcode.DISTORT_BITCRUSH, buf_out2, buf_in2, buf_bits2, buf_rate2, cedar.hash("crush2") & 0xFFFF
    )
    host2.load_instruction(inst2)
    host2.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2))

    output2 = host2.process(hf_sine)

    # The output should have aliasing artifacts due to low sample rate
    freqs = np.fft.rfftfreq(len(output2), 1/sr)
    spectrum = 20 * np.log10(np.abs(np.fft.rfft(output2)) + 1e-10)

    # Find peaks below nyquist of reduced rate
    alias_freq = sr * 0.1 / 2  # ~2.4kHz nyquist
    below_alias = freqs < alias_freq
    if np.any(below_alias):
        max_alias_level = np.max(spectrum[below_alias])
        print(f"  Effective Nyquist: {alias_freq:.0f}Hz")
        print(f"  Aliasing detected: {max_alias_level:.1f}dB")


if __name__ == "__main__":
    test_bitcrush_levels()

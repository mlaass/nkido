"""
Test: DISTORT_SOFT (Soft Clip)
==============================
Tests soft clip transfer curve with various threshold values.

Expected behavior:
- Soft clipping should smoothly limit the signal at the threshold
- Lower threshold values should clip earlier (more distortion)
- The transition from linear to clipped region should be smooth
- Output should be symmetrical for symmetrical input

If this test fails, check the implementation in cedar/include/cedar/opcodes/distortion.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import scipy.io.wavfile

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_linear_ramp, save_wav
from visualize import save_figure

OUT = output_dir("op_softclip")


def test_softclip_transfer_curves():
    """
    Test DISTORT_SOFT transfer curves at multiple threshold values.
    Lower threshold should clip earlier.
    """
    print("Test: DISTORT_SOFT Transfer Curves")

    thresh_values = [0.2, 0.4, 0.6, 0.8]
    ramp = gen_linear_ramp(2048)

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_SOFT (Soft Clip) Transfer Curves")

    for thresh, ax in zip(thresh_values, axes.flat):
        host = CedarTestHost()

        buf_in = 0
        buf_thresh = host.set_param("thresh", thresh)

        host.load_instruction(cedar.Instruction.make_binary(
            cedar.Opcode.DISTORT_SOFT, 1, buf_in, buf_thresh, cedar.hash("dist") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Verify symmetry
        mid = len(output) // 2
        pos_half = output[mid:]
        neg_half = -output[mid::-1]
        symmetry_error = np.max(np.abs(pos_half[:len(neg_half)] - neg_half[:len(pos_half)]))

        # Plot
        ax.plot(ramp, output, linewidth=2, label=f'Thresh {thresh}')
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label='Linear')
        ax.axhline(y=thresh, color='r', linestyle=':', alpha=0.4, label=f'Threshold')
        ax.axhline(y=-thresh, color='r', linestyle=':', alpha=0.4)
        ax.set_title(f'Threshold = {thresh}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.1, 1.1)
        ax.legend(loc='upper left', fontsize=8)

        print(f"  Thresh {thresh}: symmetry error = {symmetry_error:.6f}")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "softclip_transfer_curves.png"))
    print(f"  Saved {os.path.join(OUT, 'softclip_transfer_curves.png')}")


def test_softclip_wav():
    """
    Apply soft clipping to a sine wave and save WAV for human evaluation.
    """
    print("Test: DISTORT_SOFT WAV Output")

    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.8

    thresh_values = [0.3, 0.5, 0.7]

    for thresh in thresh_values:
        host = CedarTestHost(sr)

        buf_in = 0
        buf_thresh = host.set_param("thresh", thresh)

        host.load_instruction(cedar.Instruction.make_binary(
            cedar.Opcode.DISTORT_SOFT, 1, buf_in, buf_thresh, cedar.hash("dist") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        wav_path = os.path.join(OUT, f"softclip_thresh_{thresh:.1f}.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for soft clipping at threshold={thresh}")


if __name__ == "__main__":
    test_softclip_transfer_curves()
    test_softclip_wav()

"""
Test: DISTORT_TANH (Saturate)
=============================
Tests tanh saturation transfer curve with various drive values.

Expected behavior:
- Tanh distortion should smoothly compress the signal toward +/-1
- Higher drive values should increase saturation (more compression)
- Output should always be bounded to (-1, 1)
- At drive=1, gentle compression; at drive=10, near hard-clipping

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

OUT = output_dir("op_saturate")


def test_saturate_transfer_curves():
    """
    Test DISTORT_TANH transfer curves at multiple drive levels.
    Higher drive should produce more aggressive saturation.
    """
    print("Test: DISTORT_TANH Transfer Curves")

    drive_values = [1.0, 2.0, 5.0, 10.0]
    ramp = gen_linear_ramp(2048)

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_TANH (Saturate) Transfer Curves")

    for drive, ax in zip(drive_values, axes.flat):
        host = CedarTestHost()

        buf_in = 0
        buf_drive = host.set_param("drive", drive)

        host.load_instruction(cedar.Instruction.make_binary(
            cedar.Opcode.DISTORT_TANH, 1, buf_in, buf_drive, cedar.hash("dist") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Verify output is bounded
        max_abs = np.max(np.abs(output))
        bounded = max_abs <= 1.0 + 1e-6

        # Plot
        ax.plot(ramp, output, linewidth=2, label=f'Drive {drive}')
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label='Linear')
        ax.set_title(f'Drive = {drive}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.1, 1.1)
        ax.legend(loc='upper left', fontsize=8)

        status = "PASS" if bounded else "FAIL"
        print(f"  Drive {drive}: max|output| = {max_abs:.4f} - {status}")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "saturate_transfer_curves.png"))
    print(f"  Saved {os.path.join(OUT, 'saturate_transfer_curves.png')}")


def test_saturate_wav():
    """
    Apply tanh saturation to a sine wave and save WAV for human evaluation.
    """
    print("Test: DISTORT_TANH WAV Output")

    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.8

    drive_values = [1.0, 3.0, 8.0]

    for drive in drive_values:
        host = CedarTestHost(sr)

        buf_in = 0
        buf_drive = host.set_param("drive", drive)

        host.load_instruction(cedar.Instruction.make_binary(
            cedar.Opcode.DISTORT_TANH, 1, buf_in, buf_drive, cedar.hash("dist") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        wav_path = os.path.join(OUT, f"saturate_drive_{drive:.0f}.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for smooth saturation at drive={drive}")


if __name__ == "__main__":
    test_saturate_transfer_curves()
    test_saturate_wav()

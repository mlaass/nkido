"""
Test: DISTORT_TUBE (Tube Saturation)
====================================
Tests tube saturation transfer curve with various drive and bias values.

Expected behavior:
- Tube distortion should produce asymmetric saturation (warm overdrive)
- Higher drive values should increase harmonic content
- Bias parameter should shift the asymmetry of the transfer curve
- Output should introduce even harmonics due to asymmetric clipping

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

OUT = output_dir("op_tube")


def test_tube_transfer_curves():
    """
    Test DISTORT_TUBE transfer curves at multiple drive and bias values.
    Tube distortion should show asymmetric saturation.
    """
    print("Test: DISTORT_TUBE Transfer Curves")

    configs = [
        (2.0, 0.0, "Drive 2.0, Bias 0.0"),
        (5.0, 0.0, "Drive 5.0, Bias 0.0"),
        (5.0, 0.1, "Drive 5.0, Bias 0.1"),
        (10.0, 0.2, "Drive 10.0, Bias 0.2"),
    ]
    ramp = gen_linear_ramp(2048)

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_TUBE (Tube Saturation) Transfer Curves")

    for (drive, bias, label), ax in zip(configs, axes.flat):
        host = CedarTestHost()

        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_bias = host.set_param("bias", bias)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_TUBE, 1, buf_in, buf_drive, buf_bias, cedar.hash("dist") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Check for asymmetry (tube characteristic)
        pos_max = np.max(output)
        neg_min = np.min(output)
        asymmetry = abs(pos_max + neg_min)

        # Plot
        ax.plot(ramp, output, linewidth=2, label=label)
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label='Linear')
        ax.set_title(label)
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.5, 1.5)
        ax.legend(loc='upper left', fontsize=8)

        print(f"  {label}: asymmetry = {asymmetry:.4f}, range = [{neg_min:.3f}, {pos_max:.3f}]")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "tube_transfer_curves.png"))
    print(f"  Saved {os.path.join(OUT, 'tube_transfer_curves.png')}")


def test_tube_wav():
    """
    Apply tube saturation to a sine wave and save WAV for human evaluation.
    """
    print("Test: DISTORT_TUBE WAV Output")

    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.8

    configs = [
        (3.0, 0.0, "drive3_bias0"),
        (5.0, 0.1, "drive5_bias01"),
        (10.0, 0.2, "drive10_bias02"),
    ]

    for drive, bias, suffix in configs:
        host = CedarTestHost(sr)

        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_bias = host.set_param("bias", bias)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_TUBE, 1, buf_in, buf_drive, buf_bias, cedar.hash("dist") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        wav_path = os.path.join(OUT, f"tube_{suffix}.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for warm tube overdrive (drive={drive}, bias={bias})")


if __name__ == "__main__":
    test_tube_transfer_curves()
    test_tube_wav()

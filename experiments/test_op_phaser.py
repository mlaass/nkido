"""
Test: EFFECT_PHASER (Phaser)
============================
Tests phaser spectrogram with sweeping notches.

Expected behavior:
- Phaser should create moving notches in the spectrum
- LFO rate should control the speed of the sweep
- Depth parameter should control the width of the sweep
- Multiple stages should create multiple notches

If this test fails, check the implementation in cedar/include/cedar/opcodes/effects.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_white_noise, save_wav
from visualize import save_figure

OUT = output_dir("op_phaser")


def test_phaser_spectrogram():
    print("Test: Phaser Spectrogram")

    sr = 48000
    duration = 2.0

    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    # Phaser Parameters
    buf_in = 0
    buf_rate = host.set_param("rate", 1.0)  # 1 Hz sweep
    buf_depth = host.set_param("depth", 0.8)

    # Inst: Phaser(out, in, rate, depth)
    # Rate field encodes feedback (high 4) and stages (low 4)
    # Feedback ~0.5, Stages = 6
    # 0.5 maps to int 8 (approx), Stages 6
    packed_rate = (8 << 4) | 6

    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EFFECT_PHASER, 1, buf_in, buf_rate, buf_depth, cedar.hash("phaser") & 0xFFFF
    )
    inst.rate = packed_rate

    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(noise)

    # Save WAV for human evaluation
    wav_path = os.path.join(OUT, "phaser_sweep.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for sweeping notches")

    # Plot spectrogram using matplotlib directly
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.specgram(output, NFFT=1024, Fs=sr, noverlap=512, cmap='magma')
    ax.set_title("Phaser Spectrogram (6-stage, 1Hz LFO)")
    ax.set_ylabel("Frequency (Hz)")
    ax.set_xlabel("Time (s)")
    ax.set_ylim(0, 10000)

    save_figure(fig, os.path.join(OUT, "phaser_spectrogram.png"))
    print(f"  Saved {os.path.join(OUT, 'phaser_spectrogram.png')}")


if __name__ == "__main__":
    test_phaser_spectrogram()

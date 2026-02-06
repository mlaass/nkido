"""
Test: EFFECT_FLANGER (Flanger)
==============================
Tests flanger sweeping comb filter effect.

Expected behavior:
- Flanger should create moving comb filter notches in the spectrum
- The spectrogram should show periodic notch movement (sweeping)
- Rate parameter controls the LFO sweep speed
- Depth parameter controls the delay depth modulation
- Feedback should create more pronounced notches

If this test fails, check the implementation in cedar/include/cedar/opcodes/effects.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_white_noise, save_wav
from visualize import save_figure

OUT = output_dir("op_flanger")


def test_flanger_sweep():
    """
    Test flanger creates sweeping comb filter notches.
    - Input: white noise
    - Spectrogram should show periodic notch movement
    """
    print("Test: Flanger Sweep Pattern")

    sr = 48000
    duration = 4.0

    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    # Flanger parameters
    buf_in = 0
    buf_rate = host.set_param("rate", 0.5)  # 0.5 Hz sweep
    buf_depth = host.set_param("depth", 0.8)
    buf_out = 1

    # EFFECT_FLANGER: out = flanger(in, rate, depth)
    # Rate field: (feedback << 4) | mix
    feedback_int = int(0.7 * 15)  # 0.7 feedback
    mix_int = 8  # 50% mix
    packed_rate = (feedback_int << 4) | mix_int

    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EFFECT_FLANGER, buf_out, buf_in, buf_rate, buf_depth, cedar.hash("flanger") & 0xFFFF
    )
    inst.rate = packed_rate
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

    output = host.process(noise)

    # Save WAV for human evaluation
    wav_path = os.path.join(OUT, "flanger_sweep.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for sweeping comb filter effect")

    # Create spectrogram
    fig, axes = plt.subplots(2, 1, figsize=(14, 10))

    ax1 = axes[0]
    ax1.specgram(output, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax1.set_ylabel('Frequency (Hz)')
    ax1.set_xlabel('Time (s)')
    ax1.set_title('Flanger Spectrogram (0.5Hz sweep, 0.7 feedback)')
    ax1.set_ylim(0, 5000)

    # Input comparison
    ax2 = axes[1]
    ax2.specgram(noise, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax2.set_ylabel('Frequency (Hz)')
    ax2.set_xlabel('Time (s)')
    ax2.set_title('Input (White Noise) Spectrogram')
    ax2.set_ylim(0, 5000)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "flanger_spectrogram.png"))
    print(f"  Saved {os.path.join(OUT, 'flanger_spectrogram.png')}")


if __name__ == "__main__":
    test_flanger_sweep()

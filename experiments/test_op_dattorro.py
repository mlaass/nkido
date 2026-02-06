"""
Test: REVERB_DATTORRO (Dattorro Reverb)
=======================================
Tests Dattorro reverb impulse response and decay characteristics.

Expected behavior:
- Impulse response should show a smooth decay tail
- Higher decay values should produce longer reverb tails
- Predelay should introduce a gap before the reverb onset
- The decay should be roughly exponential

If this test fails, check the implementation in cedar/include/cedar/opcodes/reverb.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_impulse, save_wav
from visualize import save_figure

OUT = output_dir("op_dattorro")


def test_reverb_decay():
    print("Test: Reverb Impulse Response")

    sr = 48000
    host = CedarTestHost(sr)

    # Short impulse to trigger reverb
    impulse = gen_impulse(2.0, sr)

    # Dattorro Reverb Params
    buf_in = 0
    buf_decay = host.set_param("decay", 0.95)  # Long tail
    buf_predelay = host.set_param("predelay", 20.0)  # 20ms

    # Dattorro(out, in, decay, predelay)
    # Rate: damping | mod_depth
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.REVERB_DATTORRO, 1, buf_in, buf_decay, buf_predelay, cedar.hash("verb") & 0xFFFF
    )
    inst.rate = (0 << 4) | 0  # No mod, no damping for clear tail

    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(impulse)

    # Save WAV for human evaluation
    wav_path = os.path.join(OUT, "dattorro_impulse.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for smooth reverb tail")

    # Plot log-magnitude envelope
    time = np.arange(len(output)) / sr
    env = np.abs(output)
    env_db = 20 * np.log10(env + 1e-6)

    fig, ax = plt.subplots(figsize=(12, 6))
    ax.plot(time, env_db, linewidth=0.5, color='purple')
    ax.set_title("Dattorro Reverb Impulse Response (Decay 0.95)")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Amplitude (dB)")
    ax.set_ylim(-100, 0)
    ax.grid(True, alpha=0.3)

    save_figure(fig, os.path.join(OUT, "reverb_ir.png"))
    print(f"  Saved {os.path.join(OUT, 'reverb_ir.png')}")


if __name__ == "__main__":
    test_reverb_decay()

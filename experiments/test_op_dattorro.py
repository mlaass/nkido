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
from utils import gen_impulse, gen_noise_pulse, save_wav
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
    buf_indiff = host.set_param("input_diffusion", 0.75)
    buf_decdiff = host.set_param("decay_diffusion", 0.625)

    # Dattorro(out, in, decay, predelay, input_diffusion, decay_diffusion)
    # Rate: damping | mod_depth
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.REVERB_DATTORRO, 1, buf_in, buf_decay, buf_predelay,
        buf_indiff, buf_decdiff, cedar.hash("verb") & 0xFFFF
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


def estimate_rt60(signal, sr, threshold_db=-60):
    """Estimate RT60 from impulse response envelope."""
    env = np.abs(signal)
    window = int(0.01 * sr)
    if window > 0:
        env = np.convolve(env, np.ones(window) / window, mode='same')
    peak = np.max(env)
    if peak < 1e-10:
        return 0.0
    peak_idx = np.argmax(env)
    env_db = 20 * np.log10(env / peak + 1e-10)
    below = np.where(env_db[peak_idx:] < threshold_db)[0]
    if len(below) > 0:
        return below[0] / sr
    return (len(signal) - peak_idx) / sr


def test_tail_length():
    """
    Test reverb tail length with short noise pulse input at different decay values.

    Expected:
    - Noise pulse excites all tank modes for realistic RT60 measurement
    - RT60 increases monotonically with decay
    - Each setting produces audible reverb tail in WAV output
    """
    print("Test: REVERB_DATTORRO Tail Length (noise pulse)")

    sr = 48000
    duration = 5.0

    decay_values = [0.3, 0.6, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(len(decay_values), 1, figsize=(12, 3 * len(decay_values)))
    fig.suptitle("Dattorro Tail Length — Noise Pulse Excitation")

    for idx, decay in enumerate(decay_values):
        noise_pulse = gen_noise_pulse(duration, sr, pulse_ms=10)

        host = CedarTestHost(sr)
        buf_in = 0
        buf_decay = host.set_param("decay", decay)
        buf_predelay = host.set_param("predelay", 10.0)
        buf_indiff = host.set_param("input_diffusion", 0.75)
        buf_decdiff = host.set_param("decay_diffusion", 0.625)

        inst = cedar.Instruction.make_quinary(
            cedar.Opcode.REVERB_DATTORRO, 1, buf_in, buf_decay, buf_predelay,
            buf_indiff, buf_decdiff, cedar.hash("dat_tail") & 0xFFFF
        )
        inst.rate = (0 << 4) | 0  # No mod, no damping
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(noise_pulse)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        wav_path = os.path.join(OUT, f"dattorro_tail_decay{decay}.wav")
        save_wav(wav_path, output, sr)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        axes[idx].plot(time, env_db, linewidth=0.5)
        axes[idx].axhline(-60, color='red', linestyle='--', alpha=0.5)
        axes[idx].set_title(f'Decay={decay} (RT60≈{rt60:.2f}s)')
        axes[idx].set_ylim(-100, 0)
        axes[idx].set_ylabel('dB')
        axes[idx].grid(True, alpha=0.3)

        print(f"  Decay {decay}: RT60 ≈ {rt60:.2f}s")
        print(f"  Saved {wav_path} - Listen for reverb tail length")

    axes[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "dattorro_tail_length.png"))
    print(f"  Saved {os.path.join(OUT, 'dattorro_tail_length.png')}")

    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with decay (noise pulse)")
    else:
        print(f"  ✗ FAIL: RT60 not monotonic: {rt60s}")


if __name__ == "__main__":
    test_reverb_decay()
    test_tail_length()

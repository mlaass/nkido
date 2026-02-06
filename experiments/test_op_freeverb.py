"""
Test: REVERB_FREEVERB (Schroeder-Moorer Reverb)
=================================================
Tests Freeverb impulse response, room size, and damping.

Expected behavior:
- Impulse response should produce dense reverb tail
- Larger room_size → longer decay (higher RT60)
- Higher damping → darker reverb tail (lower spectral centroid over time)
- 8 comb filters + 4 allpass filters

If this test fails, check the implementation in cedar/include/cedar/opcodes/reverbs.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_impulse, gen_noise_pulse, save_wav
from visualize import save_figure

OUT = output_dir("op_freeverb")


def estimate_rt60(signal, sr, threshold_db=-60):
    """Estimate RT60 from impulse response envelope."""
    env = np.abs(signal)
    # Smooth envelope
    window = int(0.01 * sr)  # 10ms window
    if window > 0:
        env = np.convolve(env, np.ones(window) / window, mode='same')
    peak = np.max(env)
    if peak < 1e-10:
        return 0.0
    peak_idx = np.argmax(env)
    env_db = 20 * np.log10(env / peak + 1e-10)
    # Find where envelope drops below threshold AFTER the peak
    below = np.where(env_db[peak_idx:] < threshold_db)[0]
    if len(below) > 0:
        return below[0] / sr
    return (len(signal) - peak_idx) / sr  # Didn't decay enough


def test_impulse_response():
    """
    Test Freeverb impulse response produces dense reverb tail.

    Expected:
    - Dense, diffuse tail (not individual echoes)
    - Approximately exponential decay
    """
    print("Test: REVERB_FREEVERB Impulse Response")

    sr = 48000
    duration = 3.0

    host = CedarTestHost(sr)
    impulse = gen_impulse(duration, sr)

    buf_in = 0
    buf_room = host.set_param("room", 0.8)
    buf_damp = host.set_param("damp", 0.5)

    buf_rscale = host.set_param("room_scale", 0.28)
    buf_roffset = host.set_param("room_offset", 0.7)

    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.REVERB_FREEVERB, 1, buf_in, buf_room, buf_damp, buf_rscale, buf_roffset,
        cedar.hash("freeverb") & 0xFFFF
    )
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(impulse)

    rt60 = estimate_rt60(output, sr)
    peak_amp = np.max(np.abs(output))

    print(f"  RT60 estimate: {rt60:.2f}s")
    print(f"  Peak amplitude: {peak_amp:.4f}")

    if rt60 > 0.1:
        print("  ✓ PASS: Reverb tail present")
    else:
        print("  ✗ FAIL: No significant reverb tail")

    # Save WAV
    wav_path = os.path.join(OUT, "freeverb_impulse.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for dense reverb tail")

    # Plot
    time = np.arange(len(output)) / sr
    env_db = 20 * np.log10(np.abs(output) + 1e-10)

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))

    axes[0].plot(time, output, linewidth=0.3)
    axes[0].set_title(f"Freeverb Impulse Response (room=0.8, damp=0.5)")
    axes[0].set_xlabel("Time (s)")
    axes[0].set_ylabel("Amplitude")
    axes[0].grid(True, alpha=0.3)

    axes[1].plot(time, env_db, linewidth=0.5, color='purple')
    axes[1].axhline(-60, color='red', linestyle='--', alpha=0.5, label='RT60 threshold')
    axes[1].set_title(f"Envelope (RT60 ≈ {rt60:.2f}s)")
    axes[1].set_xlabel("Time (s)")
    axes[1].set_ylabel("Amplitude (dB)")
    axes[1].set_ylim(-100, 0)
    axes[1].legend()
    axes[1].grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_ir.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_ir.png')}")


def test_room_size():
    """
    Test room_size parameter: larger → longer decay.

    Expected:
    - RT60 increases with room_size
    """
    print("Test: REVERB_FREEVERB Room Size")

    sr = 48000
    duration = 4.0
    impulse = gen_impulse(duration, sr)

    room_values = [0.2, 0.5, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Freeverb Room Size Comparison")

    for room, ax in zip(room_values, axes.flat):
        host = CedarTestHost(sr)
        buf_in = 0
        buf_room = host.set_param("room", room)
        buf_damp = host.set_param("damp", 0.5)

        buf_rscale = host.set_param("room_scale", 0.28)
        buf_roffset = host.set_param("room_offset", 0.7)

        inst = cedar.Instruction.make_quinary(
            cedar.Opcode.REVERB_FREEVERB, 1, buf_in, buf_room, buf_damp, buf_rscale, buf_roffset,
            cedar.hash("fv_room") & 0xFFFF
        )
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(impulse)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        ax.plot(time, env_db, linewidth=0.5)
        ax.set_title(f'Room={room} (RT60≈{rt60:.2f}s)')
        ax.set_ylim(-100, 0)
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('dB')
        ax.grid(True, alpha=0.3)

        print(f"  Room {room}: RT60 ≈ {rt60:.2f}s")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_room_size.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_room_size.png')}")

    # Check monotonicity
    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with room size")
    else:
        print("  ✗ FAIL: RT60 does not increase monotonically with room size")


def test_damping():
    """
    Test damping parameter: higher → darker reverb tail.

    Expected:
    - Spectral centroid of late reverb decreases with damping
    """
    print("Test: REVERB_FREEVERB Damping")

    sr = 48000
    duration = 3.0
    impulse = gen_impulse(duration, sr)

    damp_values = [0.0, 0.3, 0.6, 0.9]
    centroids = []

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Freeverb Damping Effect on Spectrum")

    for damp, ax in zip(damp_values, axes.flat):
        host = CedarTestHost(sr)
        buf_in = 0
        buf_room = host.set_param("room", 0.8)
        buf_damp = host.set_param("damp", damp)

        buf_rscale = host.set_param("room_scale", 0.28)
        buf_roffset = host.set_param("room_offset", 0.7)

        inst = cedar.Instruction.make_quinary(
            cedar.Opcode.REVERB_FREEVERB, 1, buf_in, buf_room, buf_damp, buf_rscale, buf_roffset,
            cedar.hash("fv_damp") & 0xFFFF
        )
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(impulse)

        # Analyze late reverb tail spectrum (0.5-1.5s)
        late_start = int(0.5 * sr)
        late_end = int(1.5 * sr)
        late_tail = output[late_start:late_end]

        fft_size = min(8192, len(late_tail))
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(late_tail[:fft_size] * np.hanning(fft_size)))

        centroid = np.sum(freqs * spectrum) / (np.sum(spectrum) + 1e-10)
        centroids.append(centroid)

        spectrum_db = 20 * np.log10(spectrum + 1e-10)
        ax.semilogx(freqs[1:], spectrum_db[1:], linewidth=0.5)
        ax.set_title(f'Damp={damp} (centroid: {centroid:.0f} Hz)')
        ax.set_xlim(20, sr / 2)
        ax.set_ylim(-100, -20)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('dB')
        ax.grid(True, alpha=0.3)

        print(f"  Damp {damp}: late tail centroid = {centroid:.0f} Hz")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_damping.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_damping.png')}")

    # Centroid should decrease with damping
    is_decreasing = all(centroids[i] >= centroids[i + 1] for i in range(len(centroids) - 1))
    if is_decreasing:
        print("  ✓ PASS: Spectral centroid decreases with damping")
    else:
        print("  ⚠ WARN: Spectral centroid does not decrease monotonically with damping")


def test_tail_length():
    """
    Test reverb tail length with short noise pulse input at different room sizes.

    Expected:
    - Noise pulse excites all comb modes for realistic RT60 measurement
    - RT60 increases monotonically with room_size
    - Each setting produces audible reverb tail in WAV output
    """
    print("Test: REVERB_FREEVERB Tail Length (noise pulse)")

    sr = 48000
    duration = 5.0

    room_values = [0.2, 0.5, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(len(room_values), 1, figsize=(12, 3 * len(room_values)))
    fig.suptitle("Freeverb Tail Length — Noise Pulse Excitation")

    for idx, room in enumerate(room_values):
        noise_pulse = gen_noise_pulse(duration, sr, pulse_ms=10)

        host = CedarTestHost(sr)
        buf_in = 0
        buf_room = host.set_param("room", room)
        buf_damp = host.set_param("damp", 0.3)
        buf_rscale = host.set_param("room_scale", 0.28)
        buf_roffset = host.set_param("room_offset", 0.7)

        inst = cedar.Instruction.make_quinary(
            cedar.Opcode.REVERB_FREEVERB, 1, buf_in, buf_room, buf_damp, buf_rscale, buf_roffset,
            cedar.hash("fv_tail") & 0xFFFF
        )
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(noise_pulse)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        wav_path = os.path.join(OUT, f"freeverb_tail_room{room}.wav")
        save_wav(wav_path, output, sr)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        axes[idx].plot(time, env_db, linewidth=0.5)
        axes[idx].axhline(-60, color='red', linestyle='--', alpha=0.5)
        axes[idx].set_title(f'Room={room} (RT60≈{rt60:.2f}s)')
        axes[idx].set_ylim(-100, 0)
        axes[idx].set_ylabel('dB')
        axes[idx].grid(True, alpha=0.3)

        print(f"  Room {room}: RT60 ≈ {rt60:.2f}s")
        print(f"  Saved {wav_path} - Listen for reverb tail length")

    axes[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_tail_length.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_tail_length.png')}")

    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with room size (noise pulse)")
    else:
        print(f"  ✗ FAIL: RT60 not monotonic: {rt60s}")


if __name__ == "__main__":
    test_impulse_response()
    test_room_size()
    test_damping()
    test_tail_length()

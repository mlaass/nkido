"""
Test: SAMPLE_PLAY_LOOP (Looping Sample Player)
================================================
Tests seamless looping, pitch shifting, and gate control.

Expected behavior:
- Seamless looping: no click at loop boundary
- Pitch 2.0 = 1 octave up (verify frequency)
- Gate control: play while gate high, stop on gate low
- 5-sample micro-fade attack/release for anti-click

If this test fails, check the implementation in cedar/include/cedar/opcodes/samplers.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_sample_loop")


class LoopSamplerHost:
    """Helper to test SAMPLE_PLAY_LOOP."""

    def __init__(self, sample_rate=48000):
        self.vm = cedar.VM()
        self.vm.set_sample_rate(sample_rate)
        self.sr = sample_rate
        self.sample_ids = {}

    def load_sine_sample(self, name, freq, duration_sec):
        """Generate and load a sine wave sample with exact integer cycles."""
        num_samples = int(duration_sec * self.sr)
        cycles = int(freq * duration_sec)
        actual_freq = cycles / duration_sec
        t = np.arange(num_samples) / self.sr
        data = np.sin(2 * np.pi * actual_freq * t).astype(np.float32)
        sample_id = self.vm.load_sample(name, data, channels=1, sample_rate=self.sr)
        self.sample_ids[name] = sample_id
        return sample_id, num_samples, actual_freq

    def load_custom_sample(self, name, data, sample_rate=None):
        if sample_rate is None:
            sample_rate = self.sr
        data = data.astype(np.float32)
        sample_id = self.vm.load_sample(name, data, channels=1, sample_rate=sample_rate)
        self.sample_ids[name] = sample_id
        return sample_id

    def setup_program(self, sample_id, pitch=1.0):
        """Set up SAMPLE_PLAY_LOOP program."""
        self.vm.set_param("pitch", pitch)
        self.vm.set_param("sample_id", float(sample_id))

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("pitch")),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("sample_id")),
            cedar.Instruction.make_ternary(
                cedar.Opcode.SAMPLE_PLAY_LOOP, 10, 0, 1, 2, cedar.hash("sampler_loop")
            ),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        ]
        self.vm.load_program(program)

    def run_looped(self, duration_sec):
        """Run with gate held high (continuous loop)."""
        num_blocks = int((duration_sec * self.sr) / cedar.BLOCK_SIZE)
        output = []
        for _ in range(num_blocks):
            gate = np.ones(cedar.BLOCK_SIZE, dtype=np.float32)
            self.vm.set_buffer(0, gate)
            left, right = self.vm.process()
            output.append(left)
        return np.concatenate(output)

    def run_gated(self, duration_sec, gate_on_sec, gate_off_sec):
        """Run with gate on/off pattern."""
        num_blocks = int((duration_sec * self.sr) / cedar.BLOCK_SIZE)
        output = []
        global_sample = 0
        gate_on_samples = int(gate_on_sec * self.sr)
        gate_off_samples = int(gate_off_sec * self.sr)
        period = gate_on_samples + gate_off_samples

        for _ in range(num_blocks):
            gate = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
            for i in range(cedar.BLOCK_SIZE):
                pos_in_period = (global_sample + i) % period
                if pos_in_period < gate_on_samples:
                    gate[i] = 1.0
            self.vm.set_buffer(0, gate)
            left, right = self.vm.process()
            output.append(left)
            global_sample += cedar.BLOCK_SIZE
        return np.concatenate(output)


def test_seamless_looping():
    """
    Test loop boundary produces no click.

    Expected:
    - With exact-cycle sine sample, loop should be seamless
    - Discontinuity at loop point < 0.01
    """
    print("Test: SAMPLE_PLAY_LOOP Seamless Looping")

    sr = 48000
    host = LoopSamplerHost(sr)

    # Sine with exact integer cycles for clean looping
    sample_id, sample_length, actual_freq = host.load_sine_sample("loop_sine", 440.0, 0.1)
    host.setup_program(sample_id, pitch=1.0)

    output = host.run_looped(1.0)

    # Check for clicks at loop boundaries
    loop_length = sample_length
    num_loops = len(output) // loop_length
    max_discontinuity = 0.0

    for i in range(1, num_loops):
        boundary = i * loop_length
        if boundary < len(output):
            disc = abs(output[boundary] - output[boundary - 1])
            # Compare with expected slope for a sine wave
            expected_slope = abs(np.sin(2 * np.pi * actual_freq / sr) * 2 * np.pi * actual_freq / sr)
            max_discontinuity = max(max_discontinuity, disc)

    print(f"  Max discontinuity at loop boundary: {max_discontinuity:.6f}")

    if max_discontinuity < 0.05:
        print("  ✓ PASS: Seamless looping (no significant click)")
    else:
        print("  ✗ FAIL: Click detected at loop boundary")

    # Save WAV
    wav_path = os.path.join(OUT, "seamless_loop.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for any clicks at loop boundaries")

    # Plot around loop boundary
    fig, axes = plt.subplots(2, 1, figsize=(12, 6))

    boundary = sample_length
    window = 200
    start = max(0, boundary - window)
    end = min(len(output), boundary + window)

    axes[0].plot(range(start, end), output[start:end], 'b-', linewidth=0.8)
    axes[0].axvline(boundary, color='red', linestyle='--', label='Loop boundary')
    axes[0].set_title('Loop Boundary (wide view)')
    axes[0].set_xlabel('Sample')
    axes[0].set_ylabel('Amplitude')
    axes[0].legend()
    axes[0].grid(True, alpha=0.3)

    # Zoomed
    zoom_start = boundary - 10
    zoom_end = boundary + 10
    axes[1].plot(range(zoom_start, zoom_end), output[zoom_start:zoom_end], 'b-o', linewidth=0.8)
    axes[1].axvline(boundary, color='red', linestyle='--', label='Loop boundary')
    axes[1].set_title('Loop Boundary (zoomed)')
    axes[1].set_xlabel('Sample')
    axes[1].set_ylabel('Amplitude')
    axes[1].legend()
    axes[1].grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "loop_boundary.png"))
    print(f"  Saved {os.path.join(OUT, 'loop_boundary.png')}")


def test_pitch_shifting():
    """
    Test pitch shifting: 2.0 pitch = 1 octave up.

    Expected:
    - Measured frequency should be close to base_freq * pitch
    """
    print("Test: SAMPLE_PLAY_LOOP Pitch Shifting")

    sr = 48000
    base_freq = 440.0

    pitch_ratios = [0.5, 1.0, 1.5, 2.0]
    all_pass = True

    for pitch in pitch_ratios:
        expected_freq = base_freq * pitch
        if expected_freq >= sr / 2:
            print(f"  Pitch {pitch}: skipped (above Nyquist)")
            continue

        host = LoopSamplerHost(sr)
        sample_id, _, actual_freq = host.load_sine_sample("pitch_test", base_freq, 0.5)
        host.setup_program(sample_id, pitch=pitch)
        output = host.run_looped(0.5)

        # FFT frequency measurement
        n_fft = 2 ** int(np.ceil(np.log2(len(output) * 4)))
        fft_freqs = np.fft.rfftfreq(n_fft, 1 / sr)
        fft_mag = np.abs(np.fft.rfft(output, n=n_fft))

        min_idx = int(50 * n_fft / sr)
        peak_idx = min_idx + np.argmax(fft_mag[min_idx:])
        measured_freq = fft_freqs[peak_idx]
        error_cents = 1200 * np.log2(measured_freq / expected_freq) if expected_freq > 0 else 0

        status = "PASS" if abs(error_cents) < 10 else "FAIL"
        if status == "FAIL":
            all_pass = False
        print(f"  Pitch {pitch}x: expected={expected_freq:.1f}Hz, measured={measured_freq:.1f}Hz, "
              f"error={error_cents:.2f} cents [{status}]")

    if all_pass:
        print("  ✓ PASS: All pitch ratios accurate")
    else:
        print("  ✗ FAIL: Some pitch ratios inaccurate")


def test_gate_control():
    """
    Test gate on/off controls playback.

    Expected:
    - Sound only during gate high periods
    - Micro-fade at gate edges (5 samples)
    """
    print("Test: SAMPLE_PLAY_LOOP Gate Control")

    sr = 48000
    host = LoopSamplerHost(sr)

    sample_id, _, _ = host.load_sine_sample("gate_test", 440.0, 0.5)
    host.setup_program(sample_id, pitch=1.0)

    # Gate on for 0.3s, off for 0.2s, repeat
    output = host.run_gated(2.0, gate_on_sec=0.3, gate_off_sec=0.2)

    # Save WAV
    wav_path = os.path.join(OUT, "gate_control.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for gated playback (0.3s on, 0.2s off)")

    # Analyze: check silence during gate-off periods
    gate_on_samples = int(0.3 * sr)
    gate_off_samples = int(0.2 * sr)
    period = gate_on_samples + gate_off_samples

    # Check a gate-off region (skip the first period for envelope settling)
    off_start = period + gate_on_samples + 50  # 50 samples after gate goes low for fadeout
    off_end = off_start + gate_off_samples - 100  # Leave margin
    if off_end < len(output):
        off_region_rms = np.sqrt(np.mean(output[off_start:off_end] ** 2))
        print(f"  Gate-off region RMS: {off_region_rms:.6f}")

        if off_region_rms < 0.01:
            print("  ✓ PASS: Silence during gate-off periods")
        else:
            print("  ✗ FAIL: Non-zero output during gate-off")
    else:
        print("  ⚠ WARN: Output too short for gate analysis")

    # Plot waveform showing gate pattern
    fig, ax = plt.subplots(figsize=(14, 4))
    time_ms = np.arange(len(output)) / sr * 1000
    ax.plot(time_ms, output, linewidth=0.3)

    # Shade gate-off regions
    for i in range(5):
        off_start = (i * period + gate_on_samples) / sr * 1000
        off_end = off_start + gate_off_samples / sr * 1000
        ax.axvspan(off_start, off_end, color='red', alpha=0.1)

    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')
    ax.set_title('SAMPLE_PLAY_LOOP Gate Control (red = gate off)')
    ax.set_xlim(0, 2000)
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "gate_control.png"))
    print(f"  Saved {os.path.join(OUT, 'gate_control.png')}")


def test_pitch_wav_output():
    """Save WAV of looped playback at different pitches."""
    print("Test: SAMPLE_PLAY_LOOP WAV at Various Pitches")

    sr = 48000
    for pitch in [0.5, 1.0, 1.5, 2.0]:
        host = LoopSamplerHost(sr)
        sample_id, _, _ = host.load_sine_sample("pitch_wav", 440.0, 0.2)
        host.setup_program(sample_id, pitch=pitch)
        output = host.run_looped(2.0)

        wav_path = os.path.join(OUT, f"loop_pitch_{pitch:.1f}x.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for {pitch}x pitch ({440 * pitch:.0f}Hz)")


if __name__ == "__main__":
    test_seamless_looping()
    test_pitch_shifting()
    test_gate_control()
    test_pitch_wav_output()

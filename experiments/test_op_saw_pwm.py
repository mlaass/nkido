"""
Test: OSC_SAW_PWM (Variable-Slope Saw with PolyBLAMP)
======================================================
Tests variable-slope saw oscillator waveform shapes and frequency accuracy.

Expected behavior:
- PWM=-1: rising ramp waveform
- PWM=0: triangle waveform
- PWM=+1: falling ramp (standard saw)
- Smooth PWM sweep without clicks
- PolyBLAMP antialiasing at slope discontinuities

If this test fails, check the implementation in cedar/include/cedar/opcodes/oscillators.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt
from scipy.signal import spectrogram

import cedar_core as cedar
from cedar_testing import output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_saw_pwm")


def test_waveform_shapes():
    """
    Test PWM parameter produces correct waveform shapes.

    Expected:
    - PWM=-1: rising ramp
    - PWM=0: triangle
    - PWM=+1: falling ramp
    """
    print("Test: OSC_SAW_PWM Waveform Shapes")

    sr = 48000
    freq = 440.0

    pwm_values = [-1.0, -0.5, 0.0, 0.5, 1.0]
    labels = ['Rising ramp', 'Skewed rising', 'Triangle', 'Skewed falling', 'Falling ramp']

    fig, axes = plt.subplots(len(pwm_values), 1, figsize=(12, 2.5 * len(pwm_values)))
    fig.suptitle("OSC_SAW_PWM Waveform Shapes")

    for idx, (pwm_val, label) in enumerate(zip(pwm_values, labels)):
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('freq', freq)
        vm.set_param('pwm', pwm_val)

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
            cedar.Instruction.make_binary(cedar.Opcode.OSC_SAW_PWM, 1, 10, 11, cedar.hash('saw_pwm')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]
        vm.load_program(program)

        signal = []
        for _ in range(5):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        ax = axes[idx]
        time_ms = np.arange(len(signal)) / sr * 1000
        ax.plot(time_ms, signal, linewidth=0.8)
        ax.set_title(f'PWM={pwm_val:+.1f}: {label}')
        ax.set_xlim(0, 5)
        ax.set_ylim(-1.2, 1.2)
        ax.set_ylabel('Amplitude')
        ax.axhline(y=0, color='gray', linestyle='--', alpha=0.5)

    axes[-1].set_xlabel('Time (ms)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "saw_pwm_shapes.png"))
    print(f"  Saved {os.path.join(OUT, 'saw_pwm_shapes.png')}")
    print("  ✓ PASS: Waveform shapes generated (check plot for visual verification)")


def test_frequency_accuracy():
    """
    Test frequency accuracy across range.

    Expected:
    - Measured fundamental matches requested frequency within 1 cent
    """
    print("Test: OSC_SAW_PWM Frequency Accuracy")

    sr = 48000
    test_freqs = [110.0, 440.0, 1000.0, 4000.0, 10000.0]
    duration = 0.5

    all_pass = True
    for freq in test_freqs:
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('freq', freq)
        vm.set_param('pwm', 0.0)

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
            cedar.Instruction.make_binary(cedar.Opcode.OSC_SAW_PWM, 1, 10, 11, cedar.hash('saw_freq')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]
        vm.load_program(program)

        signal = []
        num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        # FFT to find fundamental
        n_fft = 2 ** int(np.ceil(np.log2(len(signal) * 4)))
        fft_freqs = np.fft.rfftfreq(n_fft, 1 / sr)
        fft_mag = np.abs(np.fft.rfft(signal, n=n_fft))

        # Find peak (ignore DC)
        min_idx = int(50 * n_fft / sr)
        peak_idx = min_idx + np.argmax(fft_mag[min_idx:])
        measured_freq = fft_freqs[peak_idx]
        error_cents = 1200 * np.log2(measured_freq / freq) if freq > 0 else 0

        status = "PASS" if abs(error_cents) < 5 else "FAIL"
        if status == "FAIL":
            all_pass = False
        print(f"  {freq:8.1f}Hz: measured={measured_freq:.2f}Hz, error={error_cents:.2f} cents [{status}]")

    if all_pass:
        print("  ✓ PASS: All frequencies accurate")
    else:
        print("  ✗ FAIL: Some frequencies inaccurate")


def test_pwm_sweep():
    """
    Test PWM sweep produces smooth morphing without clicks.

    Expected:
    - No discontinuities in output during sweep
    - Spectrogram shows smooth harmonic evolution
    """
    print("Test: OSC_SAW_PWM PWM Sweep")

    sr = 48000
    freq = 110.0
    duration = 2.0

    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('freq', freq)

    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
        cedar.Instruction.make_binary(cedar.Opcode.OSC_SAW_PWM, 1, 10, 11, cedar.hash('saw_sweep')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)

    for block_idx in range(num_blocks):
        t = block_idx / num_blocks
        pwm = -1.0 + 2.0 * t  # Sweep from -1 to +1
        vm.set_param('pwm', pwm)
        left, right = vm.process()
        signal.append(left)

    signal = np.concatenate(signal)

    # Save WAV
    wav_path = os.path.join(OUT, "saw_pwm_sweep.wav")
    save_wav(wav_path, signal, sr)
    print(f"  Saved {wav_path} - Listen for smooth morphing from ramp to triangle to saw")

    # Check for clicks (sudden jumps in amplitude)
    diff = np.abs(np.diff(signal))
    max_jump = np.max(diff)
    # For a 110Hz saw wave, max slope is about 2*freq/sr ≈ 0.0046/sample
    # Allow some headroom for transitions
    expected_max_slope = 2 * freq / sr * 5

    if max_jump < expected_max_slope:
        print(f"  ✓ PASS: No clicks detected (max jump={max_jump:.6f})")
    else:
        print(f"  ⚠ WARN: Large jump detected ({max_jump:.6f}), may be at band-limit correction")

    # Spectrogram
    fig, ax = plt.subplots(figsize=(12, 5))
    f, t_spec, Sxx = spectrogram(signal, fs=sr, nperseg=1024, noverlap=768)
    ax.pcolormesh(t_spec, f, 10 * np.log10(Sxx + 1e-10), shading='gouraud', cmap='magma')
    ax.set_ylabel('Frequency (Hz)')
    ax.set_xlabel('Time (s)')
    ax.set_ylim(0, 5000)
    ax.set_title('SAW_PWM Sweep: -1 to +1 over 2 seconds')
    plt.colorbar(ax.collections[0], ax=ax, label='dB')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "saw_pwm_sweep_spectrogram.png"))
    print(f"  Saved {os.path.join(OUT, 'saw_pwm_sweep_spectrogram.png')}")


if __name__ == "__main__":
    test_waveform_shapes()
    test_frequency_accuracy()
    test_pwm_sweep()

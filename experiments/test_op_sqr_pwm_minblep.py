"""
Test: OSC_SQR_PWM_MINBLEP (MinBLEP Square with PWM)
=====================================================
Tests MinBLEP square wave oscillator aliasing and duty cycle.

Expected behavior:
- MinBLEP should produce cleaner spectrum than PolyBLEP at high frequencies
- Duty cycle sweep should change harmonic content smoothly
- Sub-sample accurate edge placement via MinBLEP residual buffer

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

OUT = output_dir("op_sqr_pwm_minblep")


def generate_sqr(opcode, freq, pwm, sr, duration):
    """Generate square wave output."""
    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('freq', freq)
    vm.set_param('pwm', pwm)

    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
        cedar.Instruction.make_binary(opcode, 1, 10, 11, cedar.hash('osc')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    for _ in range(num_blocks):
        left, right = vm.process()
        signal.append(left)
    return np.concatenate(signal)


def get_noise_floor(signal, sr, fundamental_freq, fft_size=16384):
    """Measure noise floor excluding harmonics."""
    freqs = np.fft.rfftfreq(fft_size, 1 / sr)
    spectrum = np.abs(np.fft.rfft(signal[:fft_size] * np.hanning(fft_size)))
    spectrum_db = 20 * np.log10(spectrum + 1e-10)

    nyquist = sr / 2
    harmonic_mask = np.ones(len(freqs), dtype=bool)
    for h in range(1, 50):
        h_freq = fundamental_freq * h
        if h_freq > nyquist:
            break
        h_idx = int(h_freq * fft_size / sr)
        harmonic_mask[max(0, h_idx - 5):min(len(harmonic_mask), h_idx + 5)] = False

    noise_region = spectrum_db[harmonic_mask & (freqs > 100) & (freqs < nyquist - 100)]
    return np.median(noise_region) if len(noise_region) > 0 else -120


def test_duty_cycle_sweep():
    """
    Test duty cycle sweep produces smooth harmonic changes.

    Expected:
    - PWM=0: 50% duty cycle (odd harmonics only)
    - PWM→±1: narrow pulse (all harmonics, thinner sound)
    - Spectrogram shows smooth evolution
    """
    print("Test: OSC_SQR_PWM_MINBLEP Duty Cycle Sweep")

    sr = 48000
    freq = 220.0
    duration = 2.0

    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('freq', freq)

    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
        cedar.Instruction.make_binary(cedar.Opcode.OSC_SQR_PWM_MINBLEP, 1, 10, 11, cedar.hash('sqr_mb')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    for block_idx in range(num_blocks):
        t = block_idx / num_blocks
        pwm = -0.9 + 1.8 * t  # Sweep from -0.9 to +0.9
        vm.set_param('pwm', pwm)
        left, right = vm.process()
        signal.append(left)
    signal = np.concatenate(signal)

    # Save WAV
    wav_path = os.path.join(OUT, "sqr_pwm_minblep_sweep.wav")
    save_wav(wav_path, signal, sr)
    print(f"  Saved {wav_path} - Listen for smooth duty cycle sweep")

    # Spectrogram
    fig, ax = plt.subplots(figsize=(12, 5))
    f, t_spec, Sxx = spectrogram(signal, fs=sr, nperseg=1024, noverlap=768)
    ax.pcolormesh(t_spec, f, 10 * np.log10(Sxx + 1e-10), shading='gouraud', cmap='magma')
    ax.set_ylabel('Frequency (Hz)')
    ax.set_xlabel('Time (s)')
    ax.set_ylim(0, 5000)
    ax.set_title('SQR_PWM_MINBLEP Duty Cycle Sweep: -0.9 to +0.9')
    plt.colorbar(ax.collections[0], ax=ax, label='dB')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "sqr_pwm_minblep_sweep.png"))
    print(f"  Saved {os.path.join(OUT, 'sqr_pwm_minblep_sweep.png')}")
    print("  ✓ PASS: Sweep generated (check spectrogram for smooth evolution)")


def test_aliasing_comparison():
    """
    Compare MinBLEP vs PolyBLEP aliasing at high frequencies.

    Expected:
    - MinBLEP should have lower noise floor than PolyBLEP
    - Especially visible at high fundamental frequencies
    """
    print("Test: OSC_SQR_PWM_MINBLEP vs PolyBLEP Aliasing")

    sr = 48000
    duration = 0.5
    pwm = 0.0  # 50% duty cycle

    test_freqs = [440.0, 2000.0, 5000.0, 8000.0]

    fig, axes = plt.subplots(len(test_freqs), 1, figsize=(14, 3.5 * len(test_freqs)))
    fig.suptitle("MinBLEP vs PolyBLEP Square Wave Aliasing")

    for idx, freq in enumerate(test_freqs):
        sig_polyblep = generate_sqr(cedar.Opcode.OSC_SQR_PWM, freq, pwm, sr, duration)
        sig_minblep = generate_sqr(cedar.Opcode.OSC_SQR_PWM_MINBLEP, freq, pwm, sr, duration)

        floor_poly = get_noise_floor(sig_polyblep, sr, freq)
        floor_min = get_noise_floor(sig_minblep, sr, freq)
        improvement = floor_poly - floor_min

        print(f"  {freq:7.0f}Hz: PolyBLEP={floor_poly:.1f}dB, MinBLEP={floor_min:.1f}dB, "
              f"improvement={improvement:.1f}dB")

        # Plot overlay
        fft_size = 16384
        fft_freqs = np.fft.rfftfreq(fft_size, 1 / sr)

        spec_poly = 20 * np.log10(np.abs(np.fft.rfft(sig_polyblep[:fft_size] * np.hanning(fft_size))) + 1e-10)
        spec_min = 20 * np.log10(np.abs(np.fft.rfft(sig_minblep[:fft_size] * np.hanning(fft_size))) + 1e-10)

        ax = axes[idx]
        ax.plot(fft_freqs, spec_poly, linewidth=0.3, alpha=0.7, label='PolyBLEP')
        ax.plot(fft_freqs, spec_min, linewidth=0.3, alpha=0.7, label='MinBLEP')
        ax.set_title(f'{freq}Hz (improvement: {improvement:.1f}dB)')
        ax.set_xlim(0, sr / 2)
        ax.set_ylim(-120, 0)
        ax.set_ylabel('dB')
        ax.legend()
        ax.grid(True, alpha=0.3)

    axes[-1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "sqr_pwm_minblep_aliasing.png"))
    print(f"  Saved {os.path.join(OUT, 'sqr_pwm_minblep_aliasing.png')}")


def test_high_frequency():
    """
    Test at high frequency near Nyquist.

    Expected:
    - Clean output with minimal aliasing
    """
    print("Test: OSC_SQR_PWM_MINBLEP High Frequency")

    sr = 48000
    freq = 10000.0
    duration = 1.0

    signal = generate_sqr(cedar.Opcode.OSC_SQR_PWM_MINBLEP, freq, 0.0, sr, duration)

    wav_path = os.path.join(OUT, "sqr_minblep_10khz.wav")
    save_wav(wav_path, signal, sr)
    print(f"  Saved {wav_path} - Listen for clean square wave at 10kHz")

    # Measure noise floor
    floor = get_noise_floor(signal, sr, freq)
    print(f"  Noise floor at 10kHz: {floor:.1f}dB")

    if floor < -60:
        print("  ✓ PASS: Low aliasing noise at 10kHz")
    else:
        print(f"  ⚠ WARN: Noise floor {floor:.1f}dB (may be acceptable for 10kHz)")


if __name__ == "__main__":
    test_duty_cycle_sweep()
    test_aliasing_comparison()
    test_high_frequency()

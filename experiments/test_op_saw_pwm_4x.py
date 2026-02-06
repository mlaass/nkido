"""
Test: OSC_SAW_PWM_4X (4x Oversampled Variable-Slope Saw)
==========================================================
Tests aliasing reduction compared to non-oversampled version.

Expected behavior:
- Cleaner spectrum at high frequencies (5kHz+)
- Less aliasing noise above Nyquist folded back
- Same waveform shapes as OSC_SAW_PWM but cleaner
- 4x oversampled with FIR downsampling

If this test fails, check the implementation in cedar/include/cedar/opcodes/oscillators.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_saw_pwm_4x")


def generate_osc(opcode, freq, pwm, sr, duration):
    """Generate oscillator output for the given opcode."""
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


def test_aliasing_comparison():
    """
    Compare aliasing between OSC_SAW_PWM and OSC_SAW_PWM_4X.

    Expected:
    - 4X version has lower noise floor
    - Most visible at high frequencies (5kHz+)
    """
    print("Test: OSC_SAW_PWM_4X Aliasing Comparison")

    sr = 48000
    duration = 0.5

    test_freqs = [440.0, 2000.0, 5000.0, 8000.0]
    pwm = 0.5  # Asymmetric for more harmonic content

    fig, axes = plt.subplots(len(test_freqs), 2, figsize=(16, 4 * len(test_freqs)))
    fig.suptitle("SAW_PWM vs SAW_PWM_4X Aliasing Comparison")

    all_improved = True
    for idx, freq in enumerate(test_freqs):
        # Generate both versions
        sig_1x = generate_osc(cedar.Opcode.OSC_SAW_PWM, freq, pwm, sr, duration)
        sig_4x = generate_osc(cedar.Opcode.OSC_SAW_PWM_4X, freq, pwm, sr, duration)

        # Noise floor comparison
        floor_1x = get_noise_floor(sig_1x, sr, freq)
        floor_4x = get_noise_floor(sig_4x, sr, freq)
        improvement = floor_1x - floor_4x

        print(f"  {freq:7.0f}Hz: 1x floor={floor_1x:.1f}dB, 4x floor={floor_4x:.1f}dB, "
              f"improvement={improvement:.1f}dB")

        if improvement < 0:
            all_improved = False

        # Plot spectra
        fft_size = 16384
        fft_freqs = np.fft.rfftfreq(fft_size, 1 / sr)

        for col, (sig, label) in enumerate([(sig_1x, "SAW_PWM (1x)"), (sig_4x, "SAW_PWM_4X")]):
            spectrum = np.abs(np.fft.rfft(sig[:fft_size] * np.hanning(fft_size)))
            spectrum_db = 20 * np.log10(spectrum + 1e-10)

            ax = axes[idx, col]
            ax.plot(fft_freqs, spectrum_db, linewidth=0.3)
            ax.set_title(f'{label} @ {freq}Hz')
            ax.set_xlim(0, sr / 2)
            ax.set_ylim(-120, 0)
            ax.set_ylabel('dB')
            ax.grid(True, alpha=0.3)

    for ax in axes[-1]:
        ax.set_xlabel('Frequency (Hz)')

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "saw_pwm_4x_aliasing.png"))
    print(f"  Saved {os.path.join(OUT, 'saw_pwm_4x_aliasing.png')}")

    if all_improved:
        print("  ✓ PASS: 4X version has lower noise floor at all test frequencies")
    else:
        print("  ⚠ WARN: 4X version did not improve at all frequencies (may be marginal at low freq)")


def test_high_frequency():
    """
    Test at high frequency (5kHz+) where aliasing is most visible.

    Expected:
    - Significantly cleaner spectrum with 4X oversampling
    """
    print("Test: OSC_SAW_PWM_4X High Frequency")

    sr = 48000
    freq = 8000.0
    duration = 1.0

    sig_1x = generate_osc(cedar.Opcode.OSC_SAW_PWM, freq, 1.0, sr, duration)
    sig_4x = generate_osc(cedar.Opcode.OSC_SAW_PWM_4X, freq, 1.0, sr, duration)

    # Save WAVs for listening
    wav_1x = os.path.join(OUT, "saw_pwm_8khz_1x.wav")
    wav_4x = os.path.join(OUT, "saw_pwm_8khz_4x.wav")
    save_wav(wav_1x, sig_1x, sr)
    save_wav(wav_4x, sig_4x, sr)
    print(f"  Saved {wav_1x} - Listen for aliasing artifacts")
    print(f"  Saved {wav_4x} - Should sound cleaner")

    # Spectrum comparison overlay
    fft_size = 16384
    fft_freqs = np.fft.rfftfreq(fft_size, 1 / sr)
    spec_1x = 20 * np.log10(np.abs(np.fft.rfft(sig_1x[:fft_size] * np.hanning(fft_size))) + 1e-10)
    spec_4x = 20 * np.log10(np.abs(np.fft.rfft(sig_4x[:fft_size] * np.hanning(fft_size))) + 1e-10)

    fig, ax = plt.subplots(figsize=(14, 6))
    ax.plot(fft_freqs, spec_1x, linewidth=0.3, alpha=0.7, label='SAW_PWM (1x)')
    ax.plot(fft_freqs, spec_4x, linewidth=0.3, alpha=0.7, label='SAW_PWM_4X')
    ax.set_title(f'Spectrum Comparison at {freq}Hz')
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_xlim(0, sr / 2)
    ax.set_ylim(-120, 0)
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "saw_pwm_4x_high_freq.png"))
    print(f"  Saved {os.path.join(OUT, 'saw_pwm_4x_high_freq.png')}")


if __name__ == "__main__":
    test_aliasing_comparison()
    test_high_frequency()

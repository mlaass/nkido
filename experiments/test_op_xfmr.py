"""
Test: DISTORT_XFMR (Transformer Saturation)
=============================================
Tests transformer saturation with bass emphasis.

Expected behavior:
- Bass frequencies saturate more than highs (transformer core behavior)
- bass_freq parameter controls the crossover frequency
- Higher drive increases harmonics, bass-biased
- 2x oversampled for reduced aliasing

If this test fails, check the implementation in cedar/include/cedar/opcodes/distortion.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_xfmr")


def test_bass_emphasis():
    """
    Test that bass frequencies saturate more than highs.

    Expected:
    - Feed mixed low (100Hz) + high (4000Hz) sines
    - Bass THD should be higher than treble THD
    """
    print("Test: DISTORT_XFMR Bass Emphasis")

    sr = 48000
    duration = 1.0
    t = np.arange(int(duration * sr)) / sr

    bass_freq = 100.0
    treble_freq = 4000.0

    # Mixed signal: equal amplitude bass + treble
    signal = (np.sin(2 * np.pi * bass_freq * t) * 0.4 +
              np.sin(2 * np.pi * treble_freq * t) * 0.4).astype(np.float32)

    host = CedarTestHost(sr)
    buf_in = 0
    buf_drive = host.set_param("drive", 5.0)
    buf_bass = host.set_param("bass", 5.0)
    buf_bassfreq = host.set_param("bass_freq", 60.0)

    host.load_instruction(cedar.Instruction.make_quaternary(
        cedar.Opcode.DISTORT_XFMR, 1, buf_in, buf_drive, buf_bass, buf_bassfreq,
        cedar.hash("xfmr") & 0xFFFF
    ))
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(signal)

    # Measure THD per band
    fft_size = 8192
    steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
    freqs = np.fft.rfftfreq(fft_size, 1 / sr)
    spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))

    def measure_thd(fundamental_freq):
        fund_idx = int(fundamental_freq * fft_size / sr)
        fund_power = spectrum[fund_idx] ** 2
        harm_power = 0
        for h in range(2, 8):
            h_idx = fund_idx * h
            if h_idx < len(spectrum):
                harm_power += spectrum[h_idx] ** 2
        return np.sqrt(harm_power / (fund_power + 1e-20)) * 100

    bass_thd = measure_thd(bass_freq)
    treble_thd = measure_thd(treble_freq)

    print(f"  Bass THD ({bass_freq}Hz): {bass_thd:.2f}%")
    print(f"  Treble THD ({treble_freq}Hz): {treble_thd:.2f}%")

    if bass_thd > treble_thd:
        print("  ✓ PASS: Bass has more distortion than treble (transformer characteristic)")
    else:
        print("  ✗ FAIL: Expected bass THD > treble THD")

    # Plot spectrum
    spectrum_db = 20 * np.log10(spectrum + 1e-10)
    fig, ax = plt.subplots(figsize=(14, 6))
    ax.semilogx(freqs[1:], spectrum_db[1:], linewidth=0.5)
    ax.axvline(bass_freq, color='blue', linestyle='--', alpha=0.5, label=f'Bass {bass_freq}Hz')
    ax.axvline(treble_freq, color='red', linestyle='--', alpha=0.5, label=f'Treble {treble_freq}Hz')
    ax.set_title(f'XFMR Spectrum (bass THD={bass_thd:.1f}%, treble THD={treble_thd:.1f}%)')
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_xlim(20, sr / 2)
    ax.set_ylim(-100, 0)
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "xfmr_bass_emphasis.png"))
    print(f"  Saved {os.path.join(OUT, 'xfmr_bass_emphasis.png')}")


def test_bass_freq_crossover():
    """
    Test bass_freq parameter shifts the crossover frequency.

    Expected:
    - Higher bass_freq → more of the spectrum gets bass-saturated
    """
    print("Test: DISTORT_XFMR Bass Frequency Crossover")

    sr = 48000
    duration = 1.0
    np.random.seed(42)
    noise = np.random.uniform(-0.5, 0.5, int(duration * sr)).astype(np.float32)

    bass_freq_values = [30.0, 60.0, 120.0, 250.0]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_XFMR Bass Frequency Crossover")

    for bf, ax in zip(bass_freq_values, axes.flat):
        host = CedarTestHost(sr)
        buf_in = 0
        buf_drive = host.set_param("drive", 5.0)
        buf_bass = host.set_param("bass", 8.0)
        buf_bassfreq = host.set_param("bass_freq", bf)

        host.load_instruction(cedar.Instruction.make_quaternary(
            cedar.Opcode.DISTORT_XFMR, 1, buf_in, buf_drive, buf_bass, buf_bassfreq,
            cedar.hash("xfmr_bf") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(noise)

        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        ax.semilogx(freqs[1:], spectrum_db[1:], linewidth=0.5)
        ax.axvline(bf, color='red', linestyle='--', alpha=0.7, label=f'bass_freq={bf}Hz')
        ax.set_title(f'bass_freq = {bf} Hz')
        ax.set_xlim(20, sr / 2)
        ax.set_ylim(-80, 0)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.legend()
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "xfmr_bass_freq_crossover.png"))
    print(f"  Saved {os.path.join(OUT, 'xfmr_bass_freq_crossover.png')}")


def test_drive_harmonics():
    """Test increasing drive produces more harmonics, bass-biased."""
    print("Test: DISTORT_XFMR Drive Harmonics")

    sr = 48000
    duration = 1.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 80 * t).astype(np.float32) * 0.7

    drive_values = [1.0, 3.0, 6.0, 10.0]
    thd_values = []

    for drive in drive_values:
        host = CedarTestHost(sr)
        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_bass = host.set_param("bass", 5.0)
        buf_bassfreq = host.set_param("bass_freq", 60.0)

        host.load_instruction(cedar.Instruction.make_quaternary(
            cedar.Opcode.DISTORT_XFMR, 1, buf_in, buf_drive, buf_bass, buf_bassfreq,
            cedar.hash("xfmr_drv") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))

        fund_idx = int(80 * fft_size / sr)
        fund_power = spectrum[fund_idx] ** 2
        harm_power = sum(spectrum[fund_idx * h] ** 2 for h in range(2, 10) if fund_idx * h < len(spectrum))
        thd = np.sqrt(harm_power / (fund_power + 1e-20)) * 100
        thd_values.append(thd)

        print(f"  Drive {drive:5.1f}: THD = {thd:.2f}%")

    is_monotonic = all(thd_values[i] <= thd_values[i + 1] for i in range(len(thd_values) - 1))
    if is_monotonic:
        print("  ✓ PASS: THD increases with drive")
    else:
        print("  ✗ FAIL: THD does not increase monotonically")


def test_wav_output():
    """Save WAV of bass+treble mix through xfmr."""
    print("Test: DISTORT_XFMR WAV Output")

    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr

    signal = (np.sin(2 * np.pi * 80 * t) * 0.4 +
              np.sin(2 * np.pi * 2000 * t) * 0.3).astype(np.float32)

    for drive, bass in [(3.0, 3.0), (6.0, 6.0), (10.0, 8.0)]:
        host = CedarTestHost(sr)
        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_bass = host.set_param("bass", bass)
        buf_bassfreq = host.set_param("bass_freq", 60.0)

        host.load_instruction(cedar.Instruction.make_quaternary(
            cedar.Opcode.DISTORT_XFMR, 1, buf_in, buf_drive, buf_bass, buf_bassfreq,
            cedar.hash("xfmr_wav") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(signal)
        wav_path = os.path.join(OUT, f"xfmr_d{drive:.0f}_b{bass:.0f}.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for bass-emphasized transformer saturation")


if __name__ == "__main__":
    test_bass_emphasis()
    test_bass_freq_crossover()
    test_drive_harmonics()
    test_wav_output()

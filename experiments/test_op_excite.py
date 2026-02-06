"""
Test: DISTORT_EXCITE (Aural Exciter)
=====================================
Tests aural exciter harmonic generation above frequency threshold.

Expected behavior:
- amount=0: output should equal input (passthrough)
- Harmonics generated only above the frequency threshold (high-pass filtered)
- Odd/even harmonic balance controlled by harmonic_odd/harmonic_even
- 2x oversampled

If this test fails, check the implementation in cedar/include/cedar/opcodes/distortion.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_excite")


def test_passthrough():
    """
    Test amount=0 produces passthrough.

    Expected:
    - Output should be very close to input when amount=0
    """
    print("Test: DISTORT_EXCITE Passthrough (amount=0)")

    sr = 48000
    duration = 0.5
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5

    host = CedarTestHost(sr)
    buf_in = 0
    buf_amount = host.set_param("amount", 0.0)
    buf_freq = host.set_param("freq", 3000.0)
    buf_odd = host.set_param("harm_odd", 0.4)
    buf_even = host.set_param("harm_even", 0.6)

    host.load_instruction(cedar.Instruction.make_quinary(
        cedar.Opcode.DISTORT_EXCITE, 1, buf_in, buf_amount, buf_freq, buf_odd, buf_even,
        cedar.hash("excite_pt") & 0xFFFF
    ))
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(sine_input)

    # Compare input and output (allow for initial filter transient)
    skip = int(0.05 * sr)  # Skip 50ms transient
    diff = np.max(np.abs(output[skip:] - sine_input[skip:]))

    print(f"  Max difference from input: {diff:.6f}")
    if diff < 0.01:
        print("  ✓ PASS: amount=0 produces near-passthrough")
    else:
        print("  ✗ FAIL: amount=0 does not produce passthrough")


def test_harmonic_generation():
    """
    Test harmonics are generated above the frequency threshold.

    Expected:
    - Below the cutoff freq: mostly unchanged
    - Above: additional harmonic content from nonlinear processing
    """
    print("Test: DISTORT_EXCITE Harmonic Generation")

    sr = 48000
    duration = 1.0

    # Test with sine below and above exciter frequency
    excite_freq = 3000.0

    test_cases = [
        (500.0, "below_cutoff"),   # Well below exciter freq
        (4000.0, "above_cutoff"),  # Above exciter freq
    ]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle("DISTORT_EXCITE Harmonic Generation")

    for idx, (freq, label) in enumerate(test_cases):
        t = np.arange(int(duration * sr)) / sr
        sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.5

        # Dry (no exciter)
        fft_size = 8192
        freqs_fft = np.fft.rfftfreq(fft_size, 1 / sr)
        dry_steady = sine_input[int(0.2 * sr):int(0.2 * sr) + fft_size]
        dry_spec = np.abs(np.fft.rfft(dry_steady * np.hanning(fft_size)))
        dry_db = 20 * np.log10(dry_spec + 1e-10)

        # Excited
        host = CedarTestHost(sr)
        buf_in = 0
        buf_amount = host.set_param("amount", 0.8)
        buf_freq = host.set_param("freq", excite_freq)
        buf_odd = host.set_param("harm_odd", 0.4)
        buf_even = host.set_param("harm_even", 0.6)

        host.load_instruction(cedar.Instruction.make_quinary(
            cedar.Opcode.DISTORT_EXCITE, 1, buf_in, buf_amount, buf_freq, buf_odd, buf_even,
            cedar.hash("excite_hg") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        wet_steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        wet_spec = np.abs(np.fft.rfft(wet_steady * np.hanning(fft_size)))
        wet_db = 20 * np.log10(wet_spec + 1e-10)

        # Plot comparison
        ax_dry = axes[idx, 0]
        ax_wet = axes[idx, 1]

        ax_dry.plot(freqs_fft, dry_db, linewidth=0.5)
        ax_dry.set_title(f'Dry ({label}: {freq}Hz)')
        ax_dry.set_xlim(0, sr / 2)
        ax_dry.set_ylim(-100, 0)
        ax_dry.axvline(excite_freq, color='red', linestyle='--', alpha=0.5, label='Exciter freq')
        ax_dry.legend()
        ax_dry.grid(True, alpha=0.3)

        ax_wet.plot(freqs_fft, wet_db, linewidth=0.5, color='orange')
        ax_wet.set_title(f'Excited ({label}: {freq}Hz, amount=0.8)')
        ax_wet.set_xlim(0, sr / 2)
        ax_wet.set_ylim(-100, 0)
        ax_wet.axvline(excite_freq, color='red', linestyle='--', alpha=0.5, label='Exciter freq')
        ax_wet.legend()
        ax_wet.grid(True, alpha=0.3)

        # Measure added harmonic energy above exciter freq
        above_idx = int(excite_freq * fft_size / sr)
        dry_energy = np.sum(dry_spec[above_idx:] ** 2)
        wet_energy = np.sum(wet_spec[above_idx:] ** 2)
        energy_ratio = wet_energy / (dry_energy + 1e-20)

        print(f"  {label} ({freq}Hz): HF energy ratio wet/dry = {energy_ratio:.2f}x")

    for ax in axes.flat:
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "excite_harmonics.png"))
    print(f"  Saved {os.path.join(OUT, 'excite_harmonics.png')}")


def test_odd_even_balance():
    """
    Test odd/even harmonic balance parameters.

    Expected:
    - harmonic_odd dominates: stronger 3rd, 5th, 7th harmonics
    - harmonic_even dominates: stronger 2nd, 4th, 6th harmonics
    """
    print("Test: DISTORT_EXCITE Odd/Even Harmonic Balance")

    sr = 48000
    duration = 1.0
    freq = 1000.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.5

    configs = [
        (0.8, 0.2, "Odd-dominant"),
        (0.5, 0.5, "Balanced"),
        (0.2, 0.8, "Even-dominant"),
    ]

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))
    fig.suptitle("DISTORT_EXCITE Odd/Even Harmonic Balance")

    for (odd, even, label), ax in zip(configs, axes):
        host = CedarTestHost(sr)
        buf_in = 0
        buf_amount = host.set_param("amount", 0.8)
        buf_freq = host.set_param("freq", 500.0)  # Low cutoff to affect 1kHz
        buf_odd = host.set_param("harm_odd", odd)
        buf_even = host.set_param("harm_even", even)

        host.load_instruction(cedar.Instruction.make_quinary(
            cedar.Opcode.DISTORT_EXCITE, 1, buf_in, buf_amount, buf_freq, buf_odd, buf_even,
            cedar.hash("excite_oe") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs_fft = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        fund_idx = int(freq * fft_size / sr)
        fund_db = spectrum_db[fund_idx]

        # Measure odd and even harmonics
        odd_levels = []
        even_levels = []
        for h in range(2, 8):
            h_idx = fund_idx * h
            if h_idx < len(spectrum_db):
                level = spectrum_db[h_idx] - fund_db
                if h % 2 == 1:
                    odd_levels.append(level)
                else:
                    even_levels.append(level)

        avg_odd = np.mean(odd_levels) if odd_levels else -100
        avg_even = np.mean(even_levels) if even_levels else -100

        print(f"  {label} (odd={odd}, even={even}): avg_odd={avg_odd:.1f}dB, avg_even={avg_even:.1f}dB")

        mask = (freqs_fft > 500) & (freqs_fft < 10000)
        ax.plot(freqs_fft[mask], spectrum_db[mask], linewidth=0.5)
        ax.set_title(f'{label}\nodd={avg_odd:.1f}dB, even={avg_even:.1f}dB')
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.set_ylim(-100, 0)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "excite_odd_even.png"))
    print(f"  Saved {os.path.join(OUT, 'excite_odd_even.png')}")


def test_wav_output():
    """Save WAV of broadband signal with exciter."""
    print("Test: DISTORT_EXCITE WAV Output")

    sr = 48000
    duration = 2.0
    np.random.seed(42)

    # Rich signal: saw-like using additive synthesis
    t = np.arange(int(duration * sr)) / sr
    signal = np.zeros(int(duration * sr), dtype=np.float32)
    for h in range(1, 10):
        signal += (np.sin(2 * np.pi * 220 * h * t) / h).astype(np.float32)
    signal *= 0.3

    for amount in [0.3, 0.6, 1.0]:
        host = CedarTestHost(sr)
        buf_in = 0
        buf_amount = host.set_param("amount", amount)
        buf_freq = host.set_param("freq", 3000.0)
        buf_odd = host.set_param("harm_odd", 0.4)
        buf_even = host.set_param("harm_even", 0.6)

        host.load_instruction(cedar.Instruction.make_quinary(
            cedar.Opcode.DISTORT_EXCITE, 1, buf_in, buf_amount, buf_freq, buf_odd, buf_even,
            cedar.hash("excite_wav") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(signal)
        wav_path = os.path.join(OUT, f"excite_amount{amount:.1f}.wav")
        save_wav(wav_path, output, sr)
        print(f"  Saved {wav_path} - Listen for added brightness/presence (amount={amount})")


if __name__ == "__main__":
    test_passthrough()
    test_harmonic_generation()
    test_odd_even_balance()
    test_wav_output()

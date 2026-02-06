"""
Test: EFFECT_CHORUS (Chorus)
============================
Tests chorus spectral spread and pitch modulation.

Expected behavior:
- Chorus should create pitch-modulated copies of the input
- Spectral sidebands should appear around the fundamental frequency
- Rate parameter controls the LFO speed
- Depth parameter controls the amount of pitch modulation

If this test fails, check the implementation in cedar/include/cedar/opcodes/effects.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_chorus")


def test_chorus_spectrum():
    """
    Test chorus creates pitch-modulated copies.
    - Input: sine wave at 440Hz
    - Measure spectral sidebands around fundamental
    """
    print("Test: Chorus Spectral Spread")

    sr = 48000
    duration = 3.0

    host = CedarTestHost(sr)

    # Generate 440Hz sine
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5

    # Chorus parameters
    buf_in = 0
    buf_rate = host.set_param("rate", 0.5)  # 0.5 Hz LFO
    buf_depth = host.set_param("depth", 0.5)
    buf_out = 1

    # EFFECT_CHORUS: out = chorus(in, rate, depth)
    # Rate field encodes mix
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EFFECT_CHORUS, buf_out, buf_in, buf_rate, buf_depth, cedar.hash("chorus") & 0xFFFF
    )
    inst.rate = 128  # 50% wet/dry mix
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

    output = host.process(sine_input)

    # Save WAV for human evaluation
    wav_path = os.path.join(OUT, "chorus_440hz.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for gentle pitch modulation / thickening")

    # Analyze spectrum
    fft_size = 8192
    # Use steady-state portion
    steady_start = int(1.0 * sr)
    steady_output = output[steady_start:steady_start + fft_size]

    freqs = np.fft.rfftfreq(fft_size, 1/sr)
    spectrum = np.abs(np.fft.rfft(steady_output))
    spectrum_db = 20 * np.log10(spectrum + 1e-10)

    # Find fundamental and sidebands
    fundamental_idx = np.argmin(np.abs(freqs - 440))
    fundamental_level = spectrum_db[fundamental_idx]

    # Look for sidebands (detuned copies) within +-20Hz of fundamental
    sideband_region = (freqs > 420) & (freqs < 460) & (np.abs(freqs - 440) > 2)
    if np.any(sideband_region):
        sideband_level = np.max(spectrum_db[sideband_region])
        sideband_spread = np.sum(spectrum[sideband_region]) / (spectrum[fundamental_idx] + 1e-10)
        print(f"  Fundamental: {fundamental_level:.1f}dB at 440Hz")
        print(f"  Max sideband: {sideband_level:.1f}dB")
        print(f"  Spectral spread ratio: {sideband_spread:.3f}")
    else:
        print("  No sidebands detected")

    # Plot spectrum around fundamental
    fig, axes = plt.subplots(2, 1, figsize=(12, 8))

    ax1 = axes[0]
    mask = (freqs > 100) & (freqs < 1000)
    ax1.plot(freqs[mask], spectrum_db[mask], linewidth=1)
    ax1.axvline(440, color='red', linestyle='--', alpha=0.5, label='Fundamental (440Hz)')
    ax1.set_xlabel('Frequency (Hz)')
    ax1.set_ylabel('Magnitude (dB)')
    ax1.set_title('Chorus Spectrum (100-1000Hz)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Zoomed view
    ax2 = axes[1]
    mask_zoom = (freqs > 400) & (freqs < 500)
    ax2.plot(freqs[mask_zoom], spectrum_db[mask_zoom], linewidth=1)
    ax2.axvline(440, color='red', linestyle='--', alpha=0.5, label='Fundamental')
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude (dB)')
    ax2.set_title('Chorus Spectrum Detail (400-500Hz)')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "chorus_spectrum.png"))
    print(f"  Saved {os.path.join(OUT, 'chorus_spectrum.png')}")


if __name__ == "__main__":
    test_chorus_spectrum()

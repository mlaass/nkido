"""
Test: Formant Filter (FILTER_FORMANT)
======================================
Tests vowel accuracy and morph smoothness of the formant filter.

Expected behavior:
- Each vowel should have spectral peaks at the documented formant frequencies
- F1, F2, F3 accuracy should be within +/-10% of target
- Morph between vowels should be smooth (no clicks or discontinuities)

If this test fails, check the implementation in cedar/include/cedar/opcodes/filters.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import scipy.io.wavfile
from scipy.ndimage import gaussian_filter1d
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from filter_helpers import get_bode_data, get_impulse
from visualize import save_figure

OUT = output_dir("op_formant")


def test_formant_vowels():
    """
    Test FILTER_FORMANT vowel accuracy.

    Expected behavior:
    - Each vowel should have spectral peaks at the documented formant frequencies
    - F1, F2, F3 accuracy should be within +/-10% of target

    WAV files are saved for human listening evaluation.
    """
    print("Test: Formant Filter Vowel Response")

    sr = 48000
    duration = 1.0  # Longer for better audio evaluation

    # Vowel table (from C++ implementation)
    vowel_table = {
        0: ("A", [650, 1100, 2860]),
        1: ("I", [300, 2300, 3000]),
        2: ("U", [300, 870, 2240]),
        3: ("E", [400, 2000, 2550]),
        4: ("O", [400, 800, 2600]),
    }

    fig, axes = plt.subplots(3, 2, figsize=(14, 12))
    fig.suptitle('Formant Filter - Vowel Frequency Response')

    for vowel_idx in range(5):
        host = CedarTestHost(sr)

        # White noise input for spectral analysis
        noise = np.random.uniform(-0.5, 0.5, int(duration * sr)).astype(np.float32)

        buf_in = 0
        buf_vowel_a = host.set_param("vowel_a", float(vowel_idx))
        buf_vowel_b = host.set_param("vowel_b", float(vowel_idx))  # Same vowel
        buf_morph = host.set_param("morph", 0.0)  # No morphing
        buf_q = host.set_param("q", 10.0)  # Moderate resonance
        buf_out = 1

        state_id = cedar.hash(f"formant_{vowel_idx}") & 0xFFFF
        host.load_instruction(
            cedar.Instruction.make_quinary(
                cedar.Opcode.FILTER_FORMANT, buf_out, buf_in,
                buf_vowel_a, buf_vowel_b, buf_morph, buf_q, state_id
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        output = host.process(noise)

        # Save WAV for human evaluation
        vowel_name = vowel_table[vowel_idx][0]
        wav_path = os.path.join(OUT, f"formant_vowel_{vowel_name}.wav")
        scipy.io.wavfile.write(wav_path, sr, output)

        # Analyze spectrum
        fft_size = 8192
        freqs = np.fft.rfftfreq(fft_size, 1/sr)
        spectrum = np.abs(np.fft.rfft(output[:fft_size]))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Smooth spectrum for visualization
        spectrum_smooth = gaussian_filter1d(spectrum_db, sigma=10)

        ax = axes[vowel_idx // 2, vowel_idx % 2]
        vowel_name, expected_formants = vowel_table[vowel_idx]

        ax.plot(freqs, spectrum_smooth, linewidth=1, label='Response')

        # Mark expected formant frequencies
        colors = ['red', 'green', 'blue']
        for f_idx, f_expected in enumerate(expected_formants):
            ax.axvline(f_expected, color=colors[f_idx], linestyle='--', alpha=0.7,
                      label=f'F{f_idx+1}={f_expected}Hz')

        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.set_title(f'Vowel "{vowel_name}" (Index {vowel_idx})')
        ax.set_xlim(0, 5000)
        ax.legend(loc='upper right', fontsize=8)
        ax.grid(True, alpha=0.3)

        # Find actual peaks and report accuracy
        peaks_found = []
        for f_expected in expected_formants:
            # Search in window around expected
            window = (freqs > f_expected * 0.7) & (freqs < f_expected * 1.3)
            if np.any(window):
                local_peak_idx = np.argmax(spectrum[window])
                actual_freq = freqs[window][local_peak_idx]
                error = abs(actual_freq - f_expected) / f_expected * 100
                peaks_found.append((f_expected, actual_freq, error))

        # Check if any formant is off by more than 10%
        all_ok = all(err <= 10 for _, _, err in peaks_found)
        status = "✓" if all_ok else "⚠"

        print(f"  {status} Vowel '{vowel_name}': ", end="")
        for exp, act, err in peaks_found:
            print(f"F={exp}Hz->{act:.0f}Hz ({err:.1f}%), ", end="")
        print(f"[{wav_path}]")

    # Leave last subplot empty or add summary
    axes[2, 1].axis('off')
    axes[2, 1].text(0.5, 0.5, 'Vowel formants based on\naverage male voice frequencies',
                   ha='center', va='center', fontsize=12)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "formant_vowels.png"))
    print(f"  Saved {os.path.join(OUT, 'formant_vowels.png')}")


def test_formant_morph():
    """Test FILTER_FORMANT morph smoothness."""
    print("Test: Formant Filter Morph Smoothness")

    sr = 48000
    duration = 4.0  # Long duration to see morph

    host = CedarTestHost(sr)

    # White noise input
    noise = np.random.uniform(-0.3, 0.3, int(duration * sr)).astype(np.float32)

    # Morph from A (0) to I (1) over time
    # We'll create a time-varying morph by processing in chunks
    buf_in = 0
    buf_vowel_a = host.set_param("vowel_a", 0.0)  # A
    buf_vowel_b = host.set_param("vowel_b", 1.0)  # I
    buf_morph = host.set_param("morph", 0.0)  # Will be updated
    buf_q = host.set_param("q", 8.0)
    buf_out = 1

    state_id = cedar.hash("formant_morph") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_quinary(
            cedar.Opcode.FILTER_FORMANT, buf_out, buf_in,
            buf_vowel_a, buf_vowel_b, buf_morph, buf_q, state_id
        )
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    # Process in chunks, varying morph
    n_samples = len(noise)
    n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    output = []

    # Linear morph over entire duration
    for i in range(n_blocks):
        morph_val = i / n_blocks  # 0 to 1
        host.vm.set_param("morph", morph_val)

        start = i * cedar.BLOCK_SIZE
        end = min(start + cedar.BLOCK_SIZE, n_samples)
        block_in = noise[start:end]

        # Pad if needed
        if len(block_in) < cedar.BLOCK_SIZE:
            block_in = np.pad(block_in, (0, cedar.BLOCK_SIZE - len(block_in)))

        host.vm.set_buffer(0, block_in.astype(np.float32))
        l, r = host.vm.process()
        output.append(l[:end-start])

    output = np.concatenate(output)

    # Save WAV for human evaluation of morph smoothness
    wav_path = os.path.join(OUT, "formant_morph_A_to_I.wav")
    scipy.io.wavfile.write(wav_path, sr, output.astype(np.float32))

    # Create spectrogram
    fig, ax = plt.subplots(figsize=(14, 6))
    ax.specgram(output, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax.set_ylabel('Frequency (Hz)')
    ax.set_xlabel('Time (s)')
    ax.set_title('Formant Morph: A -> I (morph 0->1)')
    ax.set_ylim(0, 5000)

    # Mark expected formant transitions
    # A: F1=650, F2=1100, F3=2860
    # I: F1=300, F2=2300, F3=3000
    ax.axhline(650, color='white', linestyle='--', alpha=0.5, linewidth=0.5)
    ax.axhline(300, color='white', linestyle='--', alpha=0.5, linewidth=0.5)
    ax.axhline(1100, color='cyan', linestyle='--', alpha=0.5, linewidth=0.5)
    ax.axhline(2300, color='cyan', linestyle='--', alpha=0.5, linewidth=0.5)

    save_figure(fig, os.path.join(OUT, "formant_morph.png"))
    print(f"  Saved {os.path.join(OUT, 'formant_morph.png')}")
    print(f"  Saved {wav_path} - Listen for smooth formant transitions")


if __name__ == "__main__":
    print("=== Formant Filter (FILTER_FORMANT) Tests ===\n")
    test_formant_vowels()
    test_formant_morph()

"""
Test: Sallen-Key Filter (FILTER_SALLENKEY)
============================================
Tests the Sallen-Key filter LP/HP modes and diode clipping character.

Expected behavior:
- LP mode: lowpass response with resonance peak at cutoff
- HP mode: highpass response with resonance peak at cutoff
- Self-oscillation at high resonance (~3.5+)
- Asymmetric diode clipping character visible in transfer curve

If this test fails, check the implementation in cedar/include/cedar/opcodes/filters.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import scipy.io.wavfile
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from filter_helpers import get_bode_data, get_impulse
from visualize import save_figure

OUT = output_dir("op_sallenkey")


def test_sallenkey_modes():
    """Test FILTER_SALLENKEY lowpass and highpass modes."""
    print("Test: Sallen-Key Filter LP/HP Modes")

    sr = 48000
    cutoff = 1000.0
    resonance_values = [0.5, 1.5, 2.5, 3.5]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for mode_idx, mode in enumerate([0.0, 1.0]):  # 0=LP, 1=HP
        mode_name = "Lowpass" if mode == 0.0 else "Highpass"

        for res in resonance_values:
            host = CedarTestHost(sr)

            buf_in = 0
            buf_freq = host.set_param("cutoff", cutoff)
            buf_res = host.set_param("res", res)
            buf_mode = host.set_param("mode", mode)
            buf_out = 1

            state_id = cedar.hash(f"sallenkey_{mode}_{res}") & 0xFFFF
            host.load_instruction(
                cedar.Instruction.make_quaternary(
                    cedar.Opcode.FILTER_SALLENKEY, buf_out, buf_in,
                    buf_freq, buf_res, buf_mode, state_id
                )
            )
            host.load_instruction(
                cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
            )

            # Impulse response
            impulse = get_impulse(0.1, sr)
            response = host.process(impulse)

            # FFT
            freqs, mag_db = get_bode_data(response, sr)

            axes[mode_idx, 0].semilogx(freqs, mag_db, label=f'Res={res}')

        axes[mode_idx, 0].set_title(f'Sallen-Key {mode_name} (Fc={cutoff}Hz)')
        axes[mode_idx, 0].set_xlabel('Frequency (Hz)')
        axes[mode_idx, 0].set_ylabel('Magnitude (dB)')
        axes[mode_idx, 0].grid(True, which='both', alpha=0.3)
        axes[mode_idx, 0].legend()
        axes[mode_idx, 0].set_ylim(-60, 30)
        axes[mode_idx, 0].set_xlim(20, 20000)
        axes[mode_idx, 0].axvline(cutoff, color='gray', linestyle='--', alpha=0.5)

    # Self-oscillation test
    print("  Testing self-oscillation...")
    for mode_idx, mode in enumerate([0.0, 1.0]):
        host = CedarTestHost(sr)
        cutoff_osc = 800.0
        res_osc = 3.8

        buf_in = 0
        buf_freq = host.set_param("cutoff", cutoff_osc)
        buf_res = host.set_param("res", res_osc)
        buf_mode = host.set_param("mode", mode)
        buf_out = 1

        state_id = cedar.hash(f"sallenkey_osc_{mode}") & 0xFFFF
        host.load_instruction(
            cedar.Instruction.make_quaternary(
                cedar.Opcode.FILTER_SALLENKEY, buf_out, buf_in,
                buf_freq, buf_res, buf_mode, state_id
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Silence with kick to test self-oscillation
        silence = np.zeros(int(0.5 * sr), dtype=np.float32)
        silence[:100] = np.random.uniform(-0.3, 0.3, 100).astype(np.float32)
        output = host.process(silence)

        # Save WAV for human evaluation
        mode_name = "LP" if mode == 0.0 else "HP"
        wav_path = os.path.join(OUT, f"sallenkey_selfoscillation_{mode_name}.wav")
        scipy.io.wavfile.write(wav_path, sr, output)
        print(f"    Saved {wav_path}")

        # Time domain
        time_ms = np.arange(len(output[:4000])) / sr * 1000
        axes[mode_idx, 1].plot(time_ms, output[:4000])
        axes[mode_idx, 1].set_title(f'Sallen-Key {mode_name} Self-Oscillation (Res={res_osc})')
        axes[mode_idx, 1].set_xlabel('Time (ms)')
        axes[mode_idx, 1].set_ylabel('Amplitude')
        axes[mode_idx, 1].grid(True, alpha=0.3)

        # Check if oscillating
        steady = output[int(0.1 * sr):]
        max_amp = np.max(np.abs(steady))
        if max_amp > 0.01:
            print(f"    {mode_name}: ✓ Oscillating (max amp: {max_amp:.3f})")
        else:
            print(f"    {mode_name}: ⚠ No oscillation detected (max amp: {max_amp:.6f})")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "sallenkey_response.png"))
    print(f"  Saved {os.path.join(OUT, 'sallenkey_response.png')}")


def test_sallenkey_character():
    """Test FILTER_SALLENKEY diode clipping character."""
    print("Test: Sallen-Key Diode Character")

    sr = 48000

    # High-amplitude sine through filter with high resonance
    # Should show asymmetric clipping from diode feedback
    duration = 0.1
    freq = 200.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.8

    host = CedarTestHost(sr)

    buf_in = 0
    buf_freq = host.set_param("cutoff", 500.0)  # Above input freq
    buf_res = host.set_param("res", 3.5)  # High resonance
    buf_mode = host.set_param("mode", 0.0)  # LP
    buf_out = 1

    state_id = cedar.hash("sallenkey_char") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_quaternary(
            cedar.Opcode.FILTER_SALLENKEY, buf_out, buf_in,
            buf_freq, buf_res, buf_mode, state_id
        )
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    output = host.process(sine_input)

    # Save WAV for human evaluation of diode character
    wav_path = os.path.join(OUT, "sallenkey_character.wav")
    scipy.io.wavfile.write(wav_path, sr, output)
    print(f"  Saved {wav_path} - Listen for asymmetric distortion character")

    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    fig.suptitle('Sallen-Key Diode Character Analysis')

    # Waveform
    axes[0, 0].plot(t[:1000] * 1000, sine_input[:1000], label='Input', alpha=0.7)
    axes[0, 0].plot(t[:1000] * 1000, output[:1000], label='Output')
    axes[0, 0].set_xlabel('Time (ms)')
    axes[0, 0].set_ylabel('Amplitude')
    axes[0, 0].set_title('Waveform (First 20ms)')
    axes[0, 0].legend()
    axes[0, 0].grid(True, alpha=0.3)

    # Transfer curve (input vs output)
    # Use steady-state
    steady_in = sine_input[2000:]
    steady_out = output[2000:]
    axes[0, 1].plot(steady_in, steady_out, 'b.', markersize=0.5, alpha=0.5)
    axes[0, 1].plot([-1, 1], [-1, 1], 'k--', alpha=0.3, label='Linear')
    axes[0, 1].set_xlabel('Input')
    axes[0, 1].set_ylabel('Output')
    axes[0, 1].set_title('Transfer Curve (shows diode asymmetry)')
    axes[0, 1].set_aspect('equal')
    axes[0, 1].grid(True, alpha=0.3)

    # Spectrum comparison
    fft_size = 4096
    freqs = np.fft.rfftfreq(fft_size, 1/sr)

    spec_in = 20 * np.log10(np.abs(np.fft.rfft(sine_input[:fft_size])) + 1e-10)
    spec_out = 20 * np.log10(np.abs(np.fft.rfft(output[:fft_size])) + 1e-10)

    axes[1, 0].plot(freqs[:500], spec_in[:500], label='Input', alpha=0.7)
    axes[1, 0].plot(freqs[:500], spec_out[:500], label='Output')
    axes[1, 0].set_xlabel('Frequency (Hz)')
    axes[1, 0].set_ylabel('Magnitude (dB)')
    axes[1, 0].set_title('Spectrum (Harmonic Content)')
    axes[1, 0].legend()
    axes[1, 0].grid(True, alpha=0.3)

    # Harmonic analysis
    fundamental_idx = int(freq / (sr / fft_size))
    harmonics = []
    for h in range(1, 8):
        h_idx = fundamental_idx * h
        if h_idx < len(spec_out):
            harmonics.append(spec_out[h_idx] - spec_out[fundamental_idx])

    axes[1, 1].bar(range(1, len(harmonics)+1), harmonics)
    axes[1, 1].set_xlabel('Harmonic Number')
    axes[1, 1].set_ylabel('Level (dB rel. fundamental)')
    axes[1, 1].set_title('Harmonic Distribution')
    axes[1, 1].grid(True, alpha=0.3, axis='y')

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "sallenkey_character.png"))
    print(f"  Saved {os.path.join(OUT, 'sallenkey_character.png')}")


if __name__ == "__main__":
    print("=== Sallen-Key (FILTER_SALLENKEY) Tests ===\n")
    test_sallenkey_modes()
    test_sallenkey_character()

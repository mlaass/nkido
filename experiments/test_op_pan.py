"""
Test: PAN (Constant-Power Panning)
===================================
Tests PAN opcode for constant-power stereo panning.

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp):
- angle = (pan + 1) * PI/4, mapping pan [-1, 1] to angle [0, PI/2]
- L = mono * cos(angle), R = mono * sin(angle)
- At pan=-1: L=1, R=0; At pan=0: L=R=0.707; At pan=1: L=0, R=1
- Total power L^2 + R^2 should be constant (= mono^2)

If this test fails, check the implementation in cedar/include/cedar/opcodes/stereo.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_pan")


# =============================================================================
# Helper Functions
# =============================================================================

def gen_mono_sine(freq, duration, sr):
    """Generate a mono sine wave."""
    t = np.arange(int(duration * sr)) / sr
    return np.sin(2 * np.pi * freq * t).astype(np.float32)


# =============================================================================
# Tests
# =============================================================================

def test_pan_constant_power():
    """
    Test PAN opcode for constant-power panning.

    Acceptance criteria:
    - Power variation across pan positions < 0.5 dB
    - At pan=-1: R < -40dB relative to L
    - At pan=+1: L < -40dB relative to R
    """
    print("Test: PAN Constant-Power Panning")

    sr = 48000
    duration = 0.5

    # Test with mono sine wave
    mono = gen_mono_sine(440, duration, sr)

    # Test 21 pan positions from -1 to +1
    pan_positions = np.linspace(-1, 1, 21)
    power_measurements = []
    level_left = []
    level_right = []

    for pan_val in pan_positions:
        host = CedarTestHost(sr)

        # Buffer 0: mono input
        # Buffer 2: pan position (constant)
        buf_mono = 0
        buf_pan = host.set_param("pan", pan_val)

        # PAN: out_left=buf3, out_right=buf4 (compiler allocates consecutive)
        # PAN(out, mono, pan)
        inst = cedar.Instruction.make_binary(
            cedar.Opcode.PAN, 3, buf_mono, buf_pan, cedar.hash("pan_test") & 0xFFFF
        )
        host.load_instruction(inst)

        # Output from buffers 3 and 4
        host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 3, 4))

        out_l, out_r = host.process_stereo(mono, np.zeros_like(mono))

        # Measure RMS power
        rms_l = np.sqrt(np.mean(out_l**2))
        rms_r = np.sqrt(np.mean(out_r**2))
        total_power = rms_l**2 + rms_r**2

        power_measurements.append(total_power)
        level_left.append(rms_l)
        level_right.append(rms_r)

    # Analyze power variation
    power_measurements = np.array(power_measurements)
    mono_power = np.mean(mono**2)

    # Convert to dB relative to mono power
    power_db = 10 * np.log10(power_measurements / mono_power + 1e-10)
    power_variation = np.max(power_db) - np.min(power_db)

    # Check endpoints
    left_at_hard_left = level_left[0]
    right_at_hard_left = level_right[0]
    left_at_hard_right = level_left[-1]
    right_at_hard_right = level_right[-1]

    print(f"  Power variation across pan: {power_variation:.2f} dB")
    print(f"  At pan=-1: L={20*np.log10(left_at_hard_left+1e-10):.1f}dB, R={20*np.log10(right_at_hard_left+1e-10):.1f}dB")
    print(f"  At pan=+1: L={20*np.log10(left_at_hard_right+1e-10):.1f}dB, R={20*np.log10(right_at_hard_right+1e-10):.1f}dB")

    # Pass/fail checks
    if power_variation < 0.5:
        print(f"  ✓ PASS: Constant power maintained (variation < 0.5dB)")
    else:
        print(f"  ✗ FAIL: Power varies too much ({power_variation:.2f}dB > 0.5dB)")

    # Check hard pan isolation
    isolation_left = 20 * np.log10(right_at_hard_left / (left_at_hard_left + 1e-10) + 1e-10)
    isolation_right = 20 * np.log10(left_at_hard_right / (right_at_hard_right + 1e-10) + 1e-10)

    if isolation_left < -40 and isolation_right < -40:
        print(f"  ✓ PASS: Hard pan isolation > 40dB")
    else:
        print(f"  ✗ FAIL: Insufficient isolation at hard pan (L:{isolation_left:.1f}dB, R:{isolation_right:.1f}dB)")

    # Generate sweep WAV for human evaluation
    sweep_duration = 4.0
    mono_sweep = gen_mono_sine(440, sweep_duration, sr)

    # Create pan sweep from -1 to 1
    pan_sweep = np.linspace(-1, 1, len(mono_sweep)).astype(np.float32)

    # Process block by block with varying pan
    n_blocks = (len(mono_sweep) + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    out_left_sweep = []
    out_right_sweep = []

    for i in range(n_blocks):
        block_host = CedarTestHost(sr)
        start = i * cedar.BLOCK_SIZE
        end = min(start + cedar.BLOCK_SIZE, len(mono_sweep))
        block_len = end - start

        # Get block of mono and pan
        mono_block = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
        pan_block = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
        mono_block[:block_len] = mono_sweep[start:end]
        pan_block[:block_len] = pan_sweep[start:end]

        # Set buffers directly
        block_host.vm.set_buffer(0, mono_block)
        block_host.vm.set_buffer(2, pan_block)

        # PAN instruction
        inst = cedar.Instruction.make_binary(cedar.Opcode.PAN, 3, 0, 2, cedar.hash("pan_sweep") & 0xFFFF)
        block_host.vm.load_program([
            inst,
            cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 3, 4)
        ])

        l, r = block_host.vm.process()
        out_left_sweep.append(l[:block_len])
        out_right_sweep.append(r[:block_len])

    sweep_left = np.concatenate(out_left_sweep)
    sweep_right = np.concatenate(out_right_sweep)

    # Save stereo WAV
    stereo = np.column_stack([sweep_left, sweep_right])
    wav_path = os.path.join(OUT, "pan_sweep.wav")
    save_wav(wav_path, stereo, sr)
    print(f"  Saved {wav_path} - Listen for smooth L->R movement, no level dips at center")

    # Plot
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle("PAN Constant-Power Analysis")

    # Pan curve
    ax1 = axes[0, 0]
    ax1.plot(pan_positions, level_left, 'b-', label='Left', linewidth=2)
    ax1.plot(pan_positions, level_right, 'r-', label='Right', linewidth=2)
    ax1.set_xlabel('Pan Position')
    ax1.set_ylabel('RMS Level')
    ax1.set_title('Pan Law Curves')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(-1, 1)

    # Total power
    ax2 = axes[0, 1]
    ax2.plot(pan_positions, power_db, 'g-', linewidth=2)
    ax2.axhline(0, color='k', linestyle='--', alpha=0.5)
    ax2.set_xlabel('Pan Position')
    ax2.set_ylabel('Total Power (dB rel. mono)')
    ax2.set_title(f'Constant Power Check (variation: {power_variation:.2f}dB)')
    ax2.grid(True, alpha=0.3)
    ax2.set_xlim(-1, 1)
    ax2.set_ylim(-1, 1)

    # Sweep waveforms
    time_ms = np.arange(len(sweep_left)) / sr * 1000
    ax3 = axes[1, 0]
    ax3.plot(time_ms, sweep_left, 'b-', alpha=0.7, linewidth=0.5, label='Left')
    ax3.plot(time_ms, sweep_right, 'r-', alpha=0.7, linewidth=0.5, label='Right')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Amplitude')
    ax3.set_title('Pan Sweep Waveforms')
    ax3.legend()
    ax3.set_xlim(0, 500)  # First 500ms

    # Sweep envelope
    ax4 = axes[1, 1]
    # Compute envelope (RMS over short windows)
    window = 1024
    n_windows = len(sweep_left) // window
    env_left = [np.sqrt(np.mean(sweep_left[i*window:(i+1)*window]**2)) for i in range(n_windows)]
    env_right = [np.sqrt(np.mean(sweep_right[i*window:(i+1)*window]**2)) for i in range(n_windows)]
    env_time = np.arange(n_windows) * window / sr * 1000

    ax4.plot(env_time, env_left, 'b-', label='Left RMS', linewidth=1.5)
    ax4.plot(env_time, env_right, 'r-', label='Right RMS', linewidth=1.5)
    ax4.set_xlabel('Time (ms)')
    ax4.set_ylabel('RMS Level')
    ax4.set_title('Pan Sweep Envelope')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    fig_path = os.path.join(OUT, "pan_analysis.png")
    save_figure(fig, fig_path)
    print(f"  Saved {fig_path}")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("=" * 60)
    print("PAN OPCODE TESTS")
    print("=" * 60)

    print()
    test_pan_constant_power()

    print("\n" + "=" * 60)
    print("PAN TESTS COMPLETE")
    print("=" * 60)

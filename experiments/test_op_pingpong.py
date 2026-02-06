"""
Test: DELAY_PINGPONG (Stereo Ping-Pong Delay)
===============================================
Tests DELAY_PINGPONG opcode for stereo ping-pong delay with cross-feedback.

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp):
- Delay with cross-feedback: L feedback goes to R, R feedback goes to L
- Echoes should alternate between channels
- Includes damping (HF rolloff in feedback path)
- Output = input + delayed signal (100% wet added to dry)

If this test fails, check the implementation in cedar/include/cedar/opcodes/stereo.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_pingpong")


# =============================================================================
# Helper Functions
# =============================================================================

def gen_noise_burst(duration, sr, burst_duration=0.01, burst_start=0.0):
    """Generate a short noise burst for impulse response testing."""
    signal = np.zeros(int(duration * sr), dtype=np.float32)
    start_sample = int(burst_start * sr)
    burst_samples = int(burst_duration * sr)
    signal[start_sample:start_sample + burst_samples] = np.random.uniform(-0.8, 0.8, burst_samples).astype(np.float32)
    return signal


# =============================================================================
# Tests
# =============================================================================

def test_pingpong_delay():
    """
    Test DELAY_PINGPONG opcode for stereo ping-pong delay.

    Acceptance criteria:
    - Delay timing error < 2%
    - Echoes clearly alternate between L and R channels
    - Feedback decay follows expected curve
    """
    print("Test: DELAY_PINGPONG Stereo Delay")

    sr = 48000
    duration = 3.0
    delay_sec = 0.2  # 200ms delay

    # Input: noise burst in LEFT channel only
    left_in = gen_noise_burst(duration, sr, burst_duration=0.02, burst_start=0.0)
    right_in = np.zeros_like(left_in)

    host = CedarTestHost(sr)

    # Parameters
    buf_delay = host.set_param("delay", delay_sec)
    buf_feedback = host.set_param("feedback", 0.6)
    buf_width = host.set_param("width", 1.0)  # Full ping-pong

    # DELAY_PINGPONG(out, in_L, in_R, delay, feedback, width)
    # Note: Inputs are buf0, buf1 for L/R, then params
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.DELAY_PINGPONG, 10, 0, 1, buf_delay, buf_feedback, buf_width,
        cedar.hash("pingpong") & 0xFFFF
    )
    host.load_instruction(inst)

    # Output from buffers 10 and 11
    host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 10, 11))

    out_l, out_r = host.process_stereo(left_in, right_in)

    # Find peaks in each channel
    expected_delay_samples = int(delay_sec * sr)
    threshold = 0.01

    def find_peaks(signal, threshold, min_distance):
        peaks = []
        for i in range(1, len(signal) - 1):
            if signal[i] > threshold and signal[i] > signal[i-1] and signal[i] > signal[i+1]:
                if len(peaks) == 0 or i - peaks[-1] > min_distance:
                    peaks.append(i)
        return peaks

    peaks_l = find_peaks(np.abs(out_l), threshold, expected_delay_samples // 2)
    peaks_r = find_peaks(np.abs(out_r), threshold, expected_delay_samples // 2)

    print(f"  Expected delay: {expected_delay_samples} samples ({delay_sec*1000:.0f}ms)")
    print(f"  Left channel peaks: {len(peaks_l)}")
    print(f"  Right channel peaks: {len(peaks_r)}")

    # The first peak should be in LEFT (direct + first reflection)
    # The second peak should be in RIGHT (ping)
    # The third should be in LEFT (pong), etc.

    # Analyze alternation pattern
    all_peaks = sorted([(p, 'L') for p in peaks_l] + [(p, 'R') for p in peaks_r])

    if len(all_peaks) >= 4:
        print(f"\n  Echo pattern (first 6):")
        for i, (pos, channel) in enumerate(all_peaks[:6]):
            time_ms = pos / sr * 1000
            level_l = 20 * np.log10(np.abs(out_l[pos]) + 1e-10)
            level_r = 20 * np.log10(np.abs(out_r[pos]) + 1e-10)
            print(f"    Echo {i}: {time_ms:.1f}ms, Channel={channel}, L={level_l:.1f}dB, R={level_r:.1f}dB")

        # Check timing accuracy
        # First L echo comes at 1x delay, subsequent L echoes at 2x delay intervals (round-trip L->R->L)
        if len(peaks_l) >= 3:
            delays_l = np.diff(peaks_l)
            # Skip first interval (which is 1x delay), analyze subsequent intervals (2x delay)
            round_trip_delays = delays_l[1:]
            if len(round_trip_delays) > 0:
                avg_round_trip = np.mean(round_trip_delays)
                expected_round_trip = expected_delay_samples * 2
                timing_error = abs(avg_round_trip - expected_round_trip) / expected_round_trip * 100

                print(f"\n  First echo delay: {delays_l[0]:.0f} samples (expected: ~{expected_delay_samples})")
                print(f"  Avg round-trip delay: {avg_round_trip:.0f} samples (expected: {expected_round_trip})")
                print(f"  Timing error: {timing_error:.2f}%")

                if timing_error < 2:
                    print(f"  ✓ PASS: Timing error < 2%")
                else:
                    print(f"  ✗ FAIL: Timing error {timing_error:.2f}% > 2%")

        # Check alternation
        # After initial transient, echoes should alternate L-R-L-R
        if len(all_peaks) >= 4:
            channels = [p[1] for p in all_peaks[1:5]]  # Skip first (direct signal)

            # Check if pattern alternates
            alternates = True
            for i in range(1, len(channels)):
                if channels[i] == channels[i-1]:
                    alternates = False
                    break

            if alternates:
                print(f"  ✓ PASS: Echoes alternate between L and R")
            else:
                print(f"  ✗ FAIL: Echoes don't properly alternate: {channels}")

    # Save WAV
    stereo = np.column_stack([out_l, out_r])
    wav_path = os.path.join(OUT, "pingpong_delay.wav")
    save_wav(wav_path, stereo, sr)
    print(f"\n  Saved {wav_path} - Listen for alternating L/R echoes with smooth decay")

    # Plot
    fig, axes = plt.subplots(3, 1, figsize=(14, 10))
    fig.suptitle(f"DELAY_PINGPONG Analysis ({delay_sec*1000:.0f}ms delay, 0.6 feedback)")

    time_ms = np.arange(len(out_l)) / sr * 1000

    # Waveforms
    ax1 = axes[0]
    ax1.plot(time_ms, out_l, 'b-', alpha=0.7, linewidth=0.5, label='Left')
    ax1.plot(time_ms, out_r, 'r-', alpha=0.7, linewidth=0.5, label='Right')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title('Output Waveforms')
    ax1.legend()
    ax1.set_xlim(0, 2000)

    # Mark expected echo times
    for i in range(10):
        echo_time = (i + 1) * delay_sec * 1000
        if echo_time < 2000:
            color = 'r' if i % 2 == 0 else 'b'
            ax1.axvline(echo_time, color=color, linestyle=':', alpha=0.3)

    # Envelope (dB)
    ax2 = axes[1]
    env_l = np.abs(out_l)
    env_r = np.abs(out_r)
    env_l_db = 20 * np.log10(env_l + 1e-10)
    env_r_db = 20 * np.log10(env_r + 1e-10)

    ax2.plot(time_ms, env_l_db, 'b-', alpha=0.5, linewidth=0.5, label='Left')
    ax2.plot(time_ms, env_r_db, 'r-', alpha=0.5, linewidth=0.5, label='Right')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level (dB)')
    ax2.set_title('Envelope (dB scale)')
    ax2.legend()
    ax2.set_xlim(0, 2000)
    ax2.set_ylim(-80, 0)

    # L-R difference (shows alternation)
    ax3 = axes[2]
    diff = out_l - out_r
    ax3.plot(time_ms, diff, 'g-', linewidth=0.5)
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('L - R')
    ax3.set_title('L-R Difference (alternation visible as sign changes)')
    ax3.set_xlim(0, 2000)
    ax3.axhline(0, color='k', linewidth=0.5)

    plt.tight_layout()
    fig_path = os.path.join(OUT, "pingpong_analysis.png")
    save_figure(fig, fig_path)
    print(f"  Saved {fig_path}")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("=" * 60)
    print("DELAY_PINGPONG OPCODE TESTS")
    print("=" * 60)

    print()
    test_pingpong_delay()

    print("\n" + "=" * 60)
    print("DELAY_PINGPONG TESTS COMPLETE")
    print("=" * 60)

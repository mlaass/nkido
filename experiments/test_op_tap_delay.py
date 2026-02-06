"""
Test: DELAY_TAP + DELAY_WRITE (Tap Delay with Feedback)
========================================================
Tests tap delay timing accuracy, feedback decay, and time units.

Expected behavior:
- Impulse in → delayed impulse out at correct time
- Feedback produces repeated echoes with exponential decay
- Time units: rate=0 seconds, rate=1 ms, rate=2 samples
- Shared state between TAP and WRITE instructions

If this test fails, check the implementation in cedar/include/cedar/opcodes/delays.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_impulse, save_wav
from visualize import save_figure

OUT = output_dir("op_tap_delay")


def build_tap_delay_program(host, buf_in, buf_time, buf_feedback, state_id, time_unit=1,
                            buf_dry=None, buf_wet=None):
    """
    Build a DELAY_TAP + DELAY_WRITE program.

    The tap delay works as a pair:
    1. DELAY_TAP reads from the delay line (delayed signal)
    2. DELAY_WRITE writes the input + feedback into the delay line

    Standard signal flow:
      input → DELAY_TAP → (tap output = delayed signal)
      input + feedback*tap → DELAY_WRITE
    """
    buf_tap_out = 2  # Buffer for tap output

    # DELAY_TAP: reads delayed signal
    # inputs: [signal_in, delay_time]
    # rate = time unit
    tap_inst = cedar.Instruction.make_binary(
        cedar.Opcode.DELAY_TAP, buf_tap_out, buf_in, buf_time, state_id
    )
    tap_inst.rate = time_unit
    host.load_instruction(tap_inst)

    # DELAY_WRITE: writes input + feedback into delay line
    # inputs: [dry_input, feedback_signal, feedback_amount, dry_level, wet_level]
    if buf_dry is not None and buf_wet is not None:
        write_inst = cedar.Instruction.make_quinary(
            cedar.Opcode.DELAY_WRITE, 3, buf_in, buf_tap_out, buf_feedback,
            buf_dry, buf_wet, state_id
        )
    else:
        write_inst = cedar.Instruction.make_ternary(
            cedar.Opcode.DELAY_WRITE, 3, buf_in, buf_tap_out, buf_feedback, state_id
        )
    host.load_instruction(write_inst)

    # Output the WRITE result (mixed dry/wet)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 3))

    return buf_tap_out


def test_delay_timing_ms():
    """
    Test delay time accuracy in milliseconds.

    Expected:
    - First echo at exactly delay_time ms after input
    - Subsequent echoes at multiples of delay_time
    """
    print("Test: DELAY_TAP Timing Accuracy (ms)")

    sr = 48000
    duration = 2.0
    delay_ms = 100.0
    expected_delay_samples = int(delay_ms / 1000 * sr)
    feedback = 0.5

    host = CedarTestHost(sr)
    impulse = gen_impulse(duration, sr)

    buf_in = 0
    buf_time = host.set_param("time", delay_ms)
    buf_fb = host.set_param("feedback", feedback)

    state_id = cedar.hash("tap_delay") & 0xFFFF
    build_tap_delay_program(host, buf_in, buf_time, buf_fb, state_id, time_unit=1)

    output = host.process(impulse)

    # Find peaks
    peaks = []
    threshold = 0.01
    for i in range(1, len(output) - 1):
        if output[i] > threshold and output[i] > output[i - 1] and output[i] > output[i + 1]:
            if len(peaks) == 0 or i - peaks[-1] > expected_delay_samples // 2:
                peaks.append(i)

    # Save WAV
    wav_path = os.path.join(OUT, "tap_delay_impulse.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for evenly spaced echoes at {delay_ms}ms")

    if len(peaks) >= 2:
        delays = np.diff(peaks)
        avg_delay = np.mean(delays)
        delay_error = abs(avg_delay - expected_delay_samples) / expected_delay_samples * 100

        print(f"  Expected delay: {expected_delay_samples} samples ({delay_ms}ms)")
        print(f"  Measured avg delay: {avg_delay:.1f} samples")
        print(f"  Delay error: {delay_error:.2f}%")

        if delay_error < 2.0:
            print("  ✓ PASS: Delay timing accurate")
        else:
            print("  ✗ FAIL: Delay timing inaccurate")

        # Check feedback decay
        if len(peaks) >= 3:
            peak_levels = [output[p] for p in peaks[:5]]
            for i, (p, l) in enumerate(zip(peaks[:5], peak_levels)):
                print(f"    Echo {i}: sample={p}, level={20 * np.log10(l + 1e-10):.1f}dB")
    else:
        print(f"  ✗ FAIL: Only {len(peaks)} peaks found")

    # Plot
    fig, ax = plt.subplots(figsize=(12, 4))
    time_ms = np.arange(len(output)) / sr * 1000
    ax.plot(time_ms, output, linewidth=0.5)
    for p in peaks[:10]:
        ax.axvline(p / sr * 1000, color='red', linestyle=':', alpha=0.5)
    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')
    ax.set_title(f'Tap Delay ({delay_ms}ms, feedback={feedback})')
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "tap_delay_timing.png"))
    print(f"  Saved {os.path.join(OUT, 'tap_delay_timing.png')}")


def test_feedback_decay():
    """
    Test feedback produces correct exponential decay.

    Expected:
    - Each echo is feedback^n times the original level
    - At feedback=0.5, each echo is -6dB from previous
    """
    print("Test: DELAY_TAP Feedback Decay")

    sr = 48000
    duration = 3.0
    delay_ms = 200.0
    expected_delay_samples = int(delay_ms / 1000 * sr)

    feedback_values = [0.3, 0.5, 0.7, 0.9]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Tap Delay Feedback Decay")

    for feedback, ax in zip(feedback_values, axes.flat):
        host = CedarTestHost(sr)
        impulse = gen_impulse(duration, sr)

        buf_in = 0
        buf_time = host.set_param("time", delay_ms)
        buf_fb = host.set_param("feedback", feedback)

        state_id = cedar.hash("tap_fb") & 0xFFFF
        build_tap_delay_program(host, buf_in, buf_time, buf_fb, state_id, time_unit=1)

        output = host.process(impulse)

        time_ms = np.arange(len(output)) / sr * 1000
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        ax.plot(time_ms, env_db, linewidth=0.5)
        ax.set_title(f'Feedback={feedback}')
        ax.set_ylim(-80, 0)
        ax.set_xlabel('Time (ms)')
        ax.set_ylabel('Amplitude (dB)')
        ax.grid(True, alpha=0.3)

        # Find peaks and measure decay
        peaks = []
        threshold = 0.005
        for i in range(1, len(output) - 1):
            if output[i] > threshold and output[i] > output[i - 1] and output[i] > output[i + 1]:
                if len(peaks) == 0 or i - peaks[-1] > expected_delay_samples // 2:
                    peaks.append(i)

        if len(peaks) >= 3:
            ratios = [output[peaks[i + 1]] / (output[peaks[i]] + 1e-10) for i in range(min(4, len(peaks) - 1))]
            avg_ratio = np.mean(ratios)
            expected_ratio = feedback
            ratio_error = abs(avg_ratio - expected_ratio)
            print(f"  Feedback {feedback}: measured decay ratio = {avg_ratio:.3f} "
                  f"(expected ~{expected_ratio:.3f}, error={ratio_error:.3f})")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "tap_delay_feedback.png"))
    print(f"  Saved {os.path.join(OUT, 'tap_delay_feedback.png')}")


def test_time_units():
    """
    Test different time unit modes.

    Expected:
    - rate=0: time in seconds
    - rate=1: time in milliseconds
    - rate=2: time in samples
    All should produce the same delay for equivalent values.
    """
    print("Test: DELAY_TAP Time Units")

    sr = 48000
    duration = 1.0
    impulse = gen_impulse(duration, sr)

    # Same delay expressed in different units
    delay_seconds = 0.1
    delay_ms = 100.0
    delay_samples = float(int(0.1 * sr))

    time_configs = [
        (delay_seconds, 0, "seconds"),
        (delay_ms, 1, "milliseconds"),
        (delay_samples, 2, "samples"),
    ]

    first_echo_positions = []

    for time_val, time_unit, label in time_configs:
        host = CedarTestHost(sr)

        buf_in = 0
        buf_time = host.set_param("time", time_val)
        buf_fb = host.set_param("feedback", 0.0)

        state_id = cedar.hash("tap_tu") & 0xFFFF
        build_tap_delay_program(host, buf_in, buf_time, buf_fb, state_id, time_unit=time_unit)

        output = host.process(impulse)

        # Find first echo
        threshold = 0.01
        for i in range(int(0.01 * sr), len(output)):
            if output[i] > threshold:
                first_echo_positions.append(i)
                print(f"  {label} (value={time_val}): first echo at sample {i} ({i / sr * 1000:.1f}ms)")
                break
        else:
            first_echo_positions.append(-1)
            print(f"  {label}: no echo detected")

    # All should be at approximately the same position
    valid_positions = [p for p in first_echo_positions if p > 0]
    if len(valid_positions) >= 2:
        spread = max(valid_positions) - min(valid_positions)
        if spread < 5:  # Within 5 samples
            print("  ✓ PASS: All time units produce consistent delay")
        else:
            print(f"  ✗ FAIL: Time units produce inconsistent delays (spread={spread} samples)")
    else:
        print("  ✗ FAIL: Not enough valid echo positions detected")


if __name__ == "__main__":
    test_delay_timing_ms()
    test_feedback_decay()
    test_time_units()

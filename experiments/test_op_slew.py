"""
SLEW Opcode Quality Test (Cedar Engine)
========================================
Tests slew rate limiter rise/fall time accuracy.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder
from visualize import save_figure

OUT = output_dir("op_slew")


# =============================================================================
# SLEW Test - Rise/Fall Time Accuracy
# =============================================================================

def test_slew_timing():
    """
    Test slew rate limiter rise/fall time.
    - Step input 0->1, measure time to reach 0.99
    - Step input 1->0, measure time to reach 0.01
    - Verify matches configured rate within 1%
    """
    print("\nTest 3: SLEW (Slew Rate Limiter) Timing")
    print("=" * 60)

    sr = 48000

    # Test various slew rates (in units per second)
    slew_rates = [
        (10.0, "Fast (10/s)"),      # 100ms for full range
        (5.0, "Medium (5/s)"),      # 200ms for full range
        (2.0, "Slow (2/s)"),        # 500ms for full range
        (1.0, "Very slow (1/s)"),   # 1000ms for full range
    ]

    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(len(slew_rates), 2, figsize=(14, 4 * len(slew_rates)))

    for idx, (rate, name) in enumerate(slew_rates):
        print(f"\n  Testing {name}:")

        # Expected time to traverse 0->1 (full range)
        full_time = 1.0 / rate
        # But we measure to 99% threshold, so expected time is 99% of full time
        expected_time = 0.99 / rate
        expected_samples = int(expected_time * sr)
        duration = full_time * 2.0  # Give enough time
        num_samples = int(duration * sr)

        host = CedarTestHost(sr)

        # Set slew rate
        buf_rate = host.set_param("rate", rate)
        buf_in = 0
        buf_out = 1

        # SLEW: out = slew(in, rate)
        host.load_instruction(
            cedar.Instruction.make_binary(
                cedar.Opcode.SLEW, buf_out, buf_in, buf_rate, cedar.hash("slew") & 0xFFFF
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Create step input: 0 for first 10%, then 1
        step_start = int(0.1 * num_samples)
        step_input = np.zeros(num_samples, dtype=np.float32)
        step_input[step_start:] = 1.0

        output = host.process(step_input)

        # Measure rise time (time from step to reaching 0.99)
        rise_idx = None
        for i in range(step_start, len(output)):
            if output[i] >= 0.99:
                rise_idx = i
                break

        if rise_idx is not None:
            measured_rise_samples = rise_idx - step_start
            measured_rise_time = measured_rise_samples / sr
            rise_error_pct = (measured_rise_time - expected_time) / expected_time * 100
        else:
            measured_rise_samples = -1
            measured_rise_time = float('nan')
            rise_error_pct = float('nan')

        # Test fall time: now create 1->0 step
        host2 = CedarTestHost(sr)
        buf_rate2 = host2.set_param("rate", rate)
        buf_out2 = 1

        host2.load_instruction(
            cedar.Instruction.make_binary(
                cedar.Opcode.SLEW, buf_out2, 0, buf_rate2, cedar.hash("slew2") & 0xFFFF
            )
        )
        host2.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2)
        )

        # Start at 1, then step to 0
        step_input2 = np.ones(num_samples, dtype=np.float32)
        step_input2[step_start:] = 0.0

        output2 = host2.process(step_input2)

        # Measure fall time
        fall_idx = None
        for i in range(step_start, len(output2)):
            if output2[i] <= 0.01:
                fall_idx = i
                break

        if fall_idx is not None:
            measured_fall_samples = fall_idx - step_start
            measured_fall_time = measured_fall_samples / sr
            fall_error_pct = (measured_fall_time - expected_time) / expected_time * 100
        else:
            measured_fall_samples = -1
            measured_fall_time = float('nan')
            fall_error_pct = float('nan')

        # Check if passed
        tolerance = max(1.0, 5 / expected_samples * 100)  # 1% or ±5 samples
        rise_passed = not np.isnan(rise_error_pct) and abs(rise_error_pct) < tolerance
        fall_passed = not np.isnan(fall_error_pct) and abs(fall_error_pct) < tolerance

        test_result = {
            'name': name,
            'rate': rate,
            'expected_time_ms': expected_time * 1000,
            'rise_measured_ms': measured_rise_time * 1000 if not np.isnan(measured_rise_time) else None,
            'rise_error_pct': rise_error_pct if not np.isnan(rise_error_pct) else None,
            'rise_passed': rise_passed,
            'fall_measured_ms': measured_fall_time * 1000 if not np.isnan(measured_fall_time) else None,
            'fall_error_pct': fall_error_pct if not np.isnan(fall_error_pct) else None,
            'fall_passed': fall_passed
        }
        results['tests'].append(test_result)

        rise_status = "PASS" if rise_passed else "FAIL"
        fall_status = "PASS" if fall_passed else "FAIL"
        print(f"    Rise: expected={expected_time*1000:.1f}ms, measured={measured_rise_time*1000:.1f}ms, "
              f"error={rise_error_pct:.1f}% [{rise_status}]")
        print(f"    Fall: expected={expected_time*1000:.1f}ms, measured={measured_fall_time*1000:.1f}ms, "
              f"error={fall_error_pct:.1f}% [{fall_status}]")

        # Plot rise
        ax1 = axes[idx, 0]
        time_ms = np.arange(len(output)) / sr * 1000
        ax1.plot(time_ms, output, 'b-', linewidth=1, label='Output')
        ax1.plot(time_ms, step_input, 'g--', linewidth=0.5, alpha=0.5, label='Input')
        ax1.axhline(0.99, color='red', linestyle=':', alpha=0.5, label='99% threshold')
        if rise_idx:
            ax1.axvline(rise_idx / sr * 1000, color='red', linestyle=':', alpha=0.7)
        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Level')
        ax1.set_title(f'{name} - Rise Response')
        ax1.legend(fontsize=8)
        ax1.grid(True, alpha=0.3)

        # Plot fall
        ax2 = axes[idx, 1]
        ax2.plot(time_ms, output2, 'b-', linewidth=1, label='Output')
        ax2.plot(time_ms, step_input2, 'g--', linewidth=0.5, alpha=0.5, label='Input')
        ax2.axhline(0.01, color='red', linestyle=':', alpha=0.5, label='1% threshold')
        if fall_idx:
            ax2.axvline(fall_idx / sr * 1000, color='red', linestyle=':', alpha=0.7)
        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Level')
        ax2.set_title(f'{name} - Fall Response')
        ax2.legend(fontsize=8)
        ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'timing.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'timing.png')}")

    with open(os.path.join(OUT, 'timing.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'timing.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar SLEW Opcode Quality Test")
    print("=" * 60)
    print()

    test_slew_timing()

    print()
    print("=" * 60)
    print("SLEW test complete.")

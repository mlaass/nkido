"""
SAH Opcode Quality Test (Cedar Engine)
=======================================
Tests sample-and-hold captures value on rising edge with correct timing.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder
from visualize import save_figure

OUT = output_dir("op_sah")


# =============================================================================
# SAH Test - Sample-and-Hold Timing
# =============================================================================

def test_sah_timing():
    """
    Test sample-and-hold captures value on rising edge.
    - Ramp input with trigger at known times
    - Verify held value matches input at trigger moment
    """
    print("\nTest 4: SAH (Sample and Hold) Timing")
    print("=" * 60)

    sr = 48000
    duration = 1.0
    num_samples = int(duration * sr)

    results = {'sample_rate': sr, 'tests': []}

    # Create ramp input (linear 0 to 1 over duration)
    ramp = np.linspace(0, 1, num_samples, dtype=np.float32)

    # Test with triggers at various times
    trigger_times_ms = [100, 250, 500, 750]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for idx, trigger_ms in enumerate(trigger_times_ms):
        print(f"\n  Trigger at {trigger_ms}ms:")

        host = CedarTestHost(sr)

        # Create trigger signal (single sample pulse)
        trigger_sample = int(trigger_ms / 1000 * sr)
        trigger = np.zeros(num_samples, dtype=np.float32)
        trigger[trigger_sample] = 1.0

        # Expected held value = ramp value at trigger time
        expected_value = ramp[trigger_sample]

        buf_in = 0  # Ramp input
        buf_trig = 1  # Trigger input
        buf_out = 2

        # Set trigger to buffer 1
        # We need to inject both ramp and trigger - this is tricky with the current harness
        # SAH: out = sah(input, trigger)
        # We'll process block by block with both inputs

        # For simplicity, let's create a combined processing approach
        host.load_instruction(
            cedar.Instruction.make_binary(
                cedar.Opcode.SAH, buf_out, buf_in, buf_trig, cedar.hash("sah") & 0xFFFF
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Process manually with both inputs
        host.vm.load_program(host.program)
        n_blocks = (num_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
        padded_len = n_blocks * cedar.BLOCK_SIZE

        ramp_padded = np.zeros(padded_len, dtype=np.float32)
        ramp_padded[:num_samples] = ramp
        trigger_padded = np.zeros(padded_len, dtype=np.float32)
        trigger_padded[:num_samples] = trigger

        output = []
        for i in range(n_blocks):
            start = i * cedar.BLOCK_SIZE
            end = start + cedar.BLOCK_SIZE
            host.vm.set_buffer(0, ramp_padded[start:end])
            host.vm.set_buffer(1, trigger_padded[start:end])
            l, r = host.vm.process()
            output.append(l)

        output = np.concatenate(output)[:num_samples]

        # Verify: after trigger, output should hold the value from trigger moment
        # Check a few samples after trigger
        check_idx = trigger_sample + 100  # 100 samples after trigger
        if check_idx < len(output):
            measured_value = output[check_idx]
            error = abs(measured_value - expected_value)
            # Allow for block boundary quantization
            passed = error < 0.01 or abs(measured_value - expected_value) < cedar.BLOCK_SIZE / num_samples

            # Also check that value is stable (held)
            stability_region = output[trigger_sample + 10:trigger_sample + 500]
            stability_std = np.std(stability_region) if len(stability_region) > 0 else 0
            stable = stability_std < 0.001
        else:
            measured_value = float('nan')
            error = float('nan')
            passed = False
            stable = False

        test_result = {
            'trigger_ms': trigger_ms,
            'trigger_sample': trigger_sample,
            'expected_value': float(expected_value),
            'measured_value': float(measured_value),
            'error': float(error) if not np.isnan(error) else None,
            'passed': passed,
            'stable': stable
        }
        results['tests'].append(test_result)

        status = "PASS" if passed else "FAIL"
        stable_str = "stable" if stable else "unstable"
        print(f"    Expected hold: {expected_value:.4f}")
        print(f"    Measured hold: {measured_value:.4f}")
        print(f"    Error: {error:.6f} [{status}]")
        print(f"    Hold stability: {stable_str}")

        # Plot
        ax = axes[idx // 2, idx % 2]
        time_ms = np.arange(len(output)) / sr * 1000
        ax.plot(time_ms, ramp[:len(output)], 'g--', linewidth=0.5, alpha=0.5, label='Input (ramp)')
        ax.plot(time_ms, output, 'b-', linewidth=1, label='S&H Output')
        ax.axvline(trigger_ms, color='red', linestyle=':', alpha=0.7, label='Trigger')
        ax.axhline(expected_value, color='orange', linestyle='--', alpha=0.5,
                   label=f'Expected hold={expected_value:.3f}')
        ax.set_xlabel('Time (ms)')
        ax.set_ylabel('Level')
        ax.set_title(f'S&H Trigger at {trigger_ms}ms')
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

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
    print("Cedar SAH Opcode Quality Test")
    print("=" * 60)
    print()

    test_sah_timing()

    print()
    print("=" * 60)
    print("SAH test complete.")

"""
AR Envelope Opcode Quality Tests (Cedar Engine)
================================================
Tests for ENV_AR opcode: one-shot behavior, timing accuracy, and retriggering.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, save_wav
from visualize import save_figure
from utils import rms, ms_to_samples, samples_to_ms
from test_op_adsr import EnvelopeTestHost, measure_envelope_time

OUT = output_dir("op_ar")


# =============================================================================
# Test 3: AR Envelope (One-Shot)
# =============================================================================

def test_ar_envelope():
    """Test AR envelope one-shot behavior."""
    print("\nTest 3: AR Envelope (One-Shot)")
    print("=" * 60)

    sr = 48000

    test_configs = [
        {'attack': 0.005, 'release': 0.05, 'name': 'Percussive'},
        {'attack': 0.02, 'release': 0.1, 'name': 'Snare-like'},
        {'attack': 0.05, 'release': 0.3, 'name': 'Pluck'},
        {'attack': 0.1, 'release': 0.5, 'name': 'Slow'},
    ]

    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(len(test_configs), 2, figsize=(16, 4 * len(test_configs)))

    for idx, config in enumerate(test_configs):
        attack = config['attack']
        release = config['release']
        name = config['name']

        total_time = attack + release + 0.2
        num_samples = int(total_time * sr)

        # Single trigger pulse
        trigger = np.zeros(num_samples, dtype=np.float32)
        trigger[100] = 1.0  # Single sample trigger

        host = EnvelopeTestHost(sr)
        host.create_ar_program(attack, release, state_id=idx+100)
        output = host.run_with_gate(trigger)

        # Measure timing
        attack_result = measure_envelope_time(output, 0.99, sr, start_idx=100, direction='rising')
        peak_idx = 100 + attack_result['sample_index'] - 100
        release_result = measure_envelope_time(output, 0.01, sr, start_idx=peak_idx, direction='falling')

        # Calculate errors
        attack_error = (attack_result['time_seconds'] - attack) / attack * 100 if attack > 0 else 0
        release_error = (release_result['time_seconds'] - release) / release * 100 if release > 0 else 0

        test_result = {
            'name': name,
            'config': config,
            'attack_measured_ms': attack_result['time_ms'],
            'attack_error_pct': attack_error,
            'release_measured_ms': release_result['time_ms'],
            'release_error_pct': release_error,
            'peak_value': float(np.max(output))
        }
        results['tests'].append(test_result)

        # Check if it's one-shot (returns to zero)
        final_value = output[-1]
        is_oneshot = final_value < 0.01

        print(f"\n  {name} AR (A={attack*1000:.0f}ms, R={release*1000:.0f}ms):")
        print(f"    Attack:  expected={attack*1000:.1f}ms, measured={attack_result['time_ms']:.1f}ms, error={attack_error:.1f}%")
        print(f"    Release: expected={release*1000:.1f}ms, measured={release_result['time_ms']:.1f}ms, error={release_error:.1f}%")
        print(f"    Peak: {test_result['peak_value']:.4f}, Final: {final_value:.6f}, One-shot: {is_oneshot}")

        # Plot full envelope
        ax1 = axes[idx, 0]
        time_ms = np.arange(len(output)) / sr * 1000
        ax1.plot(time_ms, output, 'b-', linewidth=1, label='Envelope')
        ax1.axvline(100/sr*1000, color='green', linestyle='--', alpha=0.5, label='Trigger')
        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Level')
        ax1.set_title(f'{name} AR Envelope')
        ax1.legend()
        ax1.grid(True, alpha=0.3)

        # Plot zoomed attack
        ax2 = axes[idx, 1]
        zoom_end = int((attack + 0.02) * sr) + 100
        ax2.plot(time_ms[:zoom_end], output[:zoom_end], 'b-', linewidth=1)
        ax2.axvline((100/sr + attack) * 1000, color='red', linestyle='--', alpha=0.7, label=f'Expected peak')
        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Level')
        ax2.set_title(f'{name} Attack Detail')
        ax2.legend()
        ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'ar.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'ar.png')}")

    with open(os.path.join(OUT, 'ar.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'ar.json')}")

    # Test retrigger during release
    print("\n  Retrigger test:")
    host = EnvelopeTestHost(sr)
    host.create_ar_program(0.02, 0.2, state_id=200)

    trigger = np.zeros(int(0.5 * sr), dtype=np.float32)
    trigger[100] = 1.0  # First trigger
    trigger[int(0.1 * sr)] = 1.0  # Second trigger during release

    output = host.run_with_gate(trigger)

    # Find peaks
    peaks = []
    for i in range(1, len(output) - 1):
        if output[i] > output[i-1] and output[i] > output[i+1] and output[i] > 0.8:
            peaks.append(i)

    print(f"    2 triggers -> {len(peaks)} peaks detected")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    print("Cedar AR Envelope Quality Tests")
    print("=" * 60)
    print()

    test_ar_envelope()

    print()
    print("=" * 60)
    print("All AR envelope tests complete. Results saved to output/op_ar/")

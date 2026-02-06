"""
Envelope Follower Opcode Quality Tests (Cedar Engine)
=====================================================
Tests for ENV_FOLLOWER opcode: DC tracking, sine wave tracking,
asymmetric attack/release, burst tracking, and AM signal tracking.
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

OUT = output_dir("op_env_follower")


# =============================================================================
# Test 4: Envelope Follower
# =============================================================================

def test_envelope_follower():
    """Test envelope follower tracking accuracy."""
    print("\nTest 4: Envelope Follower")
    print("=" * 60)

    sr = 48000

    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(3, 2, figsize=(16, 12))

    # Test 1: DC tracking
    print("\n  DC Tracking:")
    dc_signal = np.ones(int(0.5 * sr), dtype=np.float32) * 0.8
    dc_signal[:int(0.1 * sr)] = 0  # Ramp up test

    host = EnvelopeTestHost(sr)
    host.create_follower_program(attack=0.01, release=0.1, state_id=300)
    dc_output = host.run_with_gate(dc_signal)

    # Measure tracking accuracy
    steady_start = int(0.3 * sr)
    steady_value = np.mean(dc_output[steady_start:])
    dc_error = abs(steady_value - 0.8) / 0.8 * 100

    results['tests'].append({
        'name': 'DC tracking',
        'expected': 0.8,
        'measured': float(steady_value),
        'error_pct': dc_error
    })

    print(f"    Input=0.8 DC, Output={steady_value:.4f}, Error={dc_error:.2f}%")

    ax1 = axes[0, 0]
    time_ms = np.arange(len(dc_output)) / sr * 1000
    ax1.plot(time_ms, dc_signal, 'g--', linewidth=0.5, alpha=0.5, label='Input')
    ax1.plot(time_ms, dc_output, 'b-', linewidth=1, label='Follower')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Level')
    ax1.set_title('DC Tracking')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Test 2: Sine wave tracking (rectified)
    print("\n  Sine Wave Tracking:")
    duration = 0.5
    freq = 100  # 100 Hz sine
    t = np.arange(int(duration * sr)) / sr
    sine_signal = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.9

    host2 = EnvelopeTestHost(sr)
    host2.create_follower_program(attack=0.005, release=0.02, state_id=301)
    sine_output = host2.run_with_gate(sine_signal)

    # The follower should track the rectified signal's peaks
    steady_region = sine_output[int(0.2 * sr):]
    avg_level = np.mean(steady_region)
    ripple = np.std(steady_region)

    results['tests'].append({
        'name': 'Sine tracking',
        'input_amplitude': 0.9,
        'avg_output': float(avg_level),
        'ripple': float(ripple)
    })

    print(f"    Input amplitude=0.9, Avg output={avg_level:.4f}, Ripple={ripple:.4f}")

    ax2 = axes[0, 1]
    time_ms2 = np.arange(len(sine_output)) / sr * 1000
    ax2.plot(time_ms2, np.abs(sine_signal), 'g--', linewidth=0.3, alpha=0.5, label='|Input|')
    ax2.plot(time_ms2, sine_output, 'b-', linewidth=1, label='Follower')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level')
    ax2.set_title('Sine Wave Tracking')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    # Test 3: Asymmetric attack/release
    print("\n  Asymmetric Attack/Release:")
    # Fast attack, slow release
    step_signal = np.zeros(int(0.8 * sr), dtype=np.float32)
    step_signal[int(0.1 * sr):int(0.4 * sr)] = 0.9

    host3 = EnvelopeTestHost(sr)
    host3.create_follower_program(attack=0.001, release=0.1, state_id=302)
    step_output = host3.run_with_gate(step_signal)

    # Measure attack and release times
    attack_result = measure_envelope_time(step_output, 0.8, sr, start_idx=int(0.1 * sr), direction='rising')
    release_start = int(0.4 * sr)
    release_result = measure_envelope_time(step_output, 0.1, sr, start_idx=release_start, direction='falling')

    results['tests'].append({
        'name': 'Asymmetric response',
        'attack_ms': attack_result['time_ms'],
        'release_ms': release_result['time_ms'],
        'ratio': release_result['time_seconds'] / attack_result['time_seconds'] if attack_result['time_seconds'] > 0 else 0
    })

    print(f"    Attack time: {attack_result['time_ms']:.2f}ms")
    print(f"    Release time: {release_result['time_ms']:.2f}ms")

    ax3 = axes[1, 0]
    time_ms3 = np.arange(len(step_output)) / sr * 1000
    ax3.plot(time_ms3, step_signal, 'g--', linewidth=0.5, alpha=0.5, label='Input')
    ax3.plot(time_ms3, step_output, 'b-', linewidth=1, label='Follower')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Level')
    ax3.set_title('Asymmetric Attack/Release (fast attack, slow release)')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Test 4: Burst tracking
    print("\n  Burst Tracking:")
    burst_signal = np.zeros(int(1.0 * sr), dtype=np.float32)
    # Create bursts of sine waves
    for i in range(4):
        start = int((0.1 + i * 0.2) * sr)
        end = int((0.15 + i * 0.2) * sr)
        t_burst = np.arange(end - start) / sr
        burst_signal[start:end] = np.sin(2 * np.pi * 440 * t_burst) * 0.8

    host4 = EnvelopeTestHost(sr)
    host4.create_follower_program(attack=0.001, release=0.02, state_id=303)
    burst_output = host4.run_with_gate(burst_signal)

    # Count envelope peaks
    env_peaks = []
    for i in range(1, len(burst_output) - 1):
        if burst_output[i] > burst_output[i-1] and burst_output[i] > burst_output[i+1] and burst_output[i] > 0.5:
            if len(env_peaks) == 0 or i - env_peaks[-1] > sr * 0.05:  # Minimum 50ms between peaks
                env_peaks.append(i)

    results['tests'].append({
        'name': 'Burst tracking',
        'num_bursts': 4,
        'peaks_detected': len(env_peaks)
    })

    print(f"    4 bursts -> {len(env_peaks)} envelope peaks detected")

    ax4 = axes[1, 1]
    time_ms4 = np.arange(len(burst_output)) / sr * 1000
    ax4.plot(time_ms4, np.abs(burst_signal), 'g--', linewidth=0.3, alpha=0.3, label='|Input|')
    ax4.plot(time_ms4, burst_output, 'b-', linewidth=1, label='Follower')
    ax4.set_xlabel('Time (ms)')
    ax4.set_ylabel('Level')
    ax4.set_title('Burst Tracking')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    # Test 5: Different attack/release times comparison
    print("\n  Attack/Release Time Comparison:")
    test_signal = np.zeros(int(0.5 * sr), dtype=np.float32)
    test_signal[int(0.05 * sr):int(0.25 * sr)] = 0.8

    configs = [
        {'attack': 0.001, 'release': 0.01, 'color': 'blue', 'label': 'Fast (1ms/10ms)'},
        {'attack': 0.01, 'release': 0.05, 'color': 'green', 'label': 'Medium (10ms/50ms)'},
        {'attack': 0.05, 'release': 0.2, 'color': 'red', 'label': 'Slow (50ms/200ms)'},
    ]

    ax5 = axes[2, 0]
    ax5.plot(np.arange(len(test_signal)) / sr * 1000, test_signal, 'k--', linewidth=0.5, alpha=0.5, label='Input')

    for i, cfg in enumerate(configs):
        host_cfg = EnvelopeTestHost(sr)
        host_cfg.create_follower_program(cfg['attack'], cfg['release'], state_id=310+i)
        out = host_cfg.run_with_gate(test_signal)
        ax5.plot(np.arange(len(out)) / sr * 1000, out, color=cfg['color'], linewidth=1, label=cfg['label'])

    ax5.set_xlabel('Time (ms)')
    ax5.set_ylabel('Level')
    ax5.set_title('Attack/Release Time Comparison')
    ax5.legend()
    ax5.grid(True, alpha=0.3)

    # Test 6: Audio rate tracking (amplitude modulated signal)
    print("\n  AM Signal Tracking:")
    duration = 0.3
    t = np.arange(int(duration * sr)) / sr
    carrier = np.sin(2 * np.pi * 440 * t)
    modulator = 0.5 + 0.5 * np.sin(2 * np.pi * 5 * t)  # 5 Hz modulation
    am_signal = (carrier * modulator * 0.9).astype(np.float32)

    host6 = EnvelopeTestHost(sr)
    host6.create_follower_program(attack=0.002, release=0.02, state_id=320)
    am_output = host6.run_with_gate(am_signal)

    ax6 = axes[2, 1]
    time_ms6 = np.arange(len(am_output)) / sr * 1000
    ax6.plot(time_ms6, np.abs(am_signal), 'g-', linewidth=0.2, alpha=0.3, label='|AM Signal|')
    ax6.plot(time_ms6, modulator * 0.9, 'r--', linewidth=1, alpha=0.7, label='Modulator envelope')
    ax6.plot(time_ms6, am_output, 'b-', linewidth=1, label='Follower')
    ax6.set_xlabel('Time (ms)')
    ax6.set_ylabel('Level')
    ax6.set_title('AM Signal Envelope Tracking (5 Hz modulation)')
    ax6.legend()
    ax6.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'follower.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'follower.png')}")

    with open(os.path.join(OUT, 'follower.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'follower.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    print("Cedar Envelope Follower Quality Tests")
    print("=" * 60)
    print()

    test_envelope_follower()

    print()
    print("=" * 60)
    print("All envelope follower tests complete. Results saved to output/op_env_follower/")

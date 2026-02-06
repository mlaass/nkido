"""
LFO Opcode Quality Tests (Cedar Engine)
========================================
Tests for LFO opcode: shape accuracy, frequency sync, PWM duty cycle,
and zero-crossing precision.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, save_wav
from visualize import save_figure

OUT = output_dir("op_lfo")


class SequencerTestHost:
    """Helper to run Cedar VM sequencer tests."""

    def __init__(self, sample_rate=48000, bpm=120.0):
        self.vm = cedar.VM()
        self.vm.set_sample_rate(sample_rate)
        self.vm.set_bpm(bpm)
        self.sr = sample_rate
        self.bpm = bpm
        self.program = []
        self.samples_per_beat = sample_rate * 60.0 / bpm
        self.samples_per_bar = self.samples_per_beat * 4

    def create_lfo_program(self, freq_mult: float, shape: int, duty: float = 0.5, state_id: int = 1):
        """Create LFO program.

        Args:
            freq_mult: Cycles per beat
            shape: 0=SIN, 1=TRI, 2=SAW, 3=RAMP, 4=SQR, 5=PWM, 6=SAH
            duty: Duty cycle for PWM (0-1)
            state_id: State ID
        """
        self.program = []

        self.vm.set_param("freq_mult", freq_mult)
        self.vm.set_param("duty", duty)

        # Get freq_mult into buffer 1
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("freq_mult"))
        )
        # Get duty into buffer 2
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("duty"))
        )

        # LFO: freq_mult (buf 1), duty (buf 2) -> output (buf 10)
        inst = cedar.Instruction.make_binary(cedar.Opcode.LFO, 10, 1, 2, state_id)
        inst.rate = shape
        self.program.append(inst)

        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def run(self, duration_sec: float) -> np.ndarray:
        """Run program and return output."""
        num_blocks = int((duration_sec * self.sr) / cedar.BLOCK_SIZE)
        output = []

        for _ in range(num_blocks):
            left, right = self.vm.process()
            output.append(left)

        return np.concatenate(output)


# =============================================================================
# Test 1: LFO Shape Accuracy
# =============================================================================

def test_lfo_shapes():
    """Test LFO waveform shapes."""
    print("Test 1: LFO Shape Accuracy")
    print("=" * 60)

    sr = 48000
    bpm = 120
    freq_mult = 1.0  # 1 cycle per beat

    shapes = [
        {'id': 0, 'name': 'SIN', 'expected_range': (-1, 1)},
        {'id': 1, 'name': 'TRI', 'expected_range': (-1, 1)},
        {'id': 2, 'name': 'SAW', 'expected_range': (-1, 1)},
        {'id': 3, 'name': 'RAMP', 'expected_range': (-1, 1)},
        {'id': 4, 'name': 'SQR', 'expected_range': (-1, 1)},
        {'id': 5, 'name': 'PWM', 'expected_range': (-1, 1)},
        {'id': 6, 'name': 'SAH', 'expected_range': (-1, 1)},
    ]

    results = {'sample_rate': sr, 'bpm': bpm, 'freq_mult': freq_mult, 'tests': []}

    fig, axes = plt.subplots(4, 2, figsize=(14, 14))
    axes = axes.flatten()

    for idx, shape in enumerate(shapes):
        host = SequencerTestHost(sr, bpm)
        host.create_lfo_program(freq_mult, shape['id'], duty=0.5, state_id=idx+100)

        # 2 beats
        duration = 2 * 60.0 / bpm
        output = host.run(duration)

        # Analyze waveform
        min_val = float(np.min(output))
        max_val = float(np.max(output))
        mean_val = float(np.mean(output))

        # Count zero crossings per cycle
        zero_crossings = sum(1 for i in range(1, len(output))
                           if (output[i-1] < 0 and output[i] >= 0) or
                              (output[i-1] >= 0 and output[i] < 0))

        # Measure frequency via zero crossings (each cycle has 2 crossings for bipolar)
        expected_crossings = 2 * 2  # 2 cycles, 2 crossings each

        test_result = {
            'name': shape['name'],
            'min': min_val,
            'max': max_val,
            'mean': mean_val,
            'zero_crossings': zero_crossings,
            'expected_crossings': expected_crossings,
            'bounded': bool(min_val >= -1.01 and max_val <= 1.01)
        }
        results['tests'].append(test_result)

        # Check bounds
        bounded_status = "PASS" if test_result['bounded'] else "FAIL"
        print(f"  {shape['name']:4s}: min={min_val:.3f}, max={max_val:.3f}, mean={mean_val:.3f}, crossings={zero_crossings} [{bounded_status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)
        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Value')
        ax.set_title(f'{shape["name"]} LFO (1 cycle/beat)')
        ax.set_xlim(0, 2)
        ax.set_ylim(-1.2, 1.2)
        ax.axhline(0, color='gray', linewidth=0.5)
        ax.grid(True, alpha=0.3)

        # Mark beat boundaries
        ax.axvline(1, color='red', linestyle='--', alpha=0.3)

    # Hide unused subplot
    if len(shapes) < len(axes):
        axes[-1].set_visible(False)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'lfo_shapes.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'lfo_shapes.png')}")

    with open(os.path.join(OUT, 'lfo_shapes.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'lfo_shapes.json')}")

    return results


# =============================================================================
# Test 2: LFO Frequency Sync
# =============================================================================

def test_lfo_freq_sync():
    """Test LFO frequency synchronization."""
    print("\nTest 2: LFO Frequency Sync")
    print("=" * 60)

    sr = 48000
    bpm = 120

    freq_mults = [0.25, 0.5, 1.0, 2.0, 4.0]

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    fig, axes = plt.subplots(len(freq_mults), 1, figsize=(14, 3 * len(freq_mults)))

    for idx, freq_mult in enumerate(freq_mults):
        host = SequencerTestHost(sr, bpm)
        host.create_lfo_program(freq_mult, shape=2, state_id=idx+200)  # SAW for clear cycle detection

        # 4 beats
        duration = 4 * 60.0 / bpm
        output = host.run(duration)

        # Count phase wraps (cycle completions for SAW: drops from ~1 to ~-1)
        cycle_wraps = []
        for i in range(1, len(output)):
            if output[i-1] > 0.5 and output[i] < -0.5:  # Large drop = cycle wrap
                cycle_wraps.append(i)

        expected_cycles = int(4 * freq_mult)  # 4 beats * freq_mult cycles per beat
        actual_cycles = len(cycle_wraps)

        # Analyze cycle timing
        if len(cycle_wraps) >= 2:
            intervals = np.diff(cycle_wraps)
            avg_interval = np.mean(intervals)
            expected_interval = host.samples_per_beat / freq_mult
            interval_error = (avg_interval - expected_interval) / expected_interval * 100
        else:
            avg_interval = 0
            expected_interval = host.samples_per_beat / freq_mult
            interval_error = 0

        test_result = {
            'freq_mult': freq_mult,
            'expected_cycles': expected_cycles,
            'actual_cycles': actual_cycles,
            'expected_interval_samples': expected_interval,
            'measured_interval_samples': float(avg_interval),
            'interval_error_pct': float(interval_error)
        }
        results['tests'].append(test_result)

        cycle_status = "PASS" if abs(actual_cycles - expected_cycles) <= 1 else "FAIL"
        print(f"  freq_mult={freq_mult}: expected={expected_cycles} cycles, actual={actual_cycles}, error={interval_error:.2f}% [{cycle_status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)

        # Mark cycle boundaries
        for wrap in cycle_wraps:
            ax.axvline(wrap / host.samples_per_beat, color='red', linestyle=':', alpha=0.5)

        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Value')
        ax.set_title(f'LFO SAW @ {freq_mult} cycles/beat ({actual_cycles} cycles in 4 beats)')
        ax.set_xlim(0, 4)
        ax.set_ylim(-1.2, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'lfo_sync.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'lfo_sync.png')}")

    with open(os.path.join(OUT, 'lfo_sync.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'lfo_sync.json')}")

    return results


# =============================================================================
# Test 3: LFO PWM Duty Cycle
# =============================================================================

def test_lfo_pwm():
    """Test LFO PWM duty cycle accuracy."""
    print("\nTest 3: LFO PWM Duty Cycle")
    print("=" * 60)

    sr = 48000
    bpm = 120

    duty_cycles = [0.1, 0.25, 0.5, 0.75, 0.9]

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    fig, axes = plt.subplots(len(duty_cycles), 1, figsize=(14, 2 * len(duty_cycles)))

    for idx, duty in enumerate(duty_cycles):
        host = SequencerTestHost(sr, bpm)
        host.create_lfo_program(1.0, shape=5, duty=duty, state_id=idx+300)  # PWM

        # 2 beats
        duration = 2 * 60.0 / bpm
        output = host.run(duration)

        # Measure actual duty cycle (fraction of time at +1)
        high_samples = np.sum(output > 0)
        total_samples = len(output)
        measured_duty = high_samples / total_samples

        duty_error = (measured_duty - duty) / duty * 100 if duty > 0 else 0

        test_result = {
            'expected_duty': duty,
            'measured_duty': float(measured_duty),
            'error_pct': float(duty_error)
        }
        results['tests'].append(test_result)

        status = "PASS" if abs(duty_error) < 5 else "FAIL"
        print(f"  duty={duty:.2f}: measured={measured_duty:.3f}, error={duty_error:.1f}% [{status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)
        ax.fill_between(time_beats, 0, output, where=output > 0, alpha=0.3)
        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Value')
        ax.set_title(f'PWM duty={duty:.0%} (measured={measured_duty:.1%})')
        ax.set_xlim(0, 2)
        ax.set_ylim(-1.2, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'lfo_pwm.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'lfo_pwm.png')}")

    with open(os.path.join(OUT, 'lfo_pwm.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'lfo_pwm.json')}")

    return results


# =============================================================================
# Test 4: LFO Zero-Crossing Precision
# =============================================================================

def test_lfo_zero_crossing_precision():
    """Test LFO zero-crossing timing at sample level."""
    print("\nTest 4: LFO Zero-Crossing Precision")
    print("=" * 60)

    sr = 48000
    bpm = 120
    samples_per_beat = sr * 60.0 / bpm

    results = {'sample_rate': sr, 'bpm': bpm, 'samples_per_beat': samples_per_beat, 'tests': []}

    # Test SAW LFO at 1 cycle per beat
    # SAW goes from -1 to +1, so crosses zero at phase 0.5 (mid-beat)
    # and wraps at phase 1.0 (beat boundary, going from +1 to -1)

    num_beats = 100
    duration = num_beats * 60.0 / bpm

    host = SequencerTestHost(sr, bpm)
    host.create_lfo_program(1.0, shape=2, state_id=1300)  # SAW shape
    output = host.run(duration)

    # Find positive-going zero crossings (from negative to positive)
    # For SAW: this happens at phase 0.5, i.e., mid-beat
    zero_crossings_pos = []
    for i in range(1, len(output)):
        if output[i-1] < 0 and output[i] >= 0:
            # Interpolate exact position
            t = -output[i-1] / (output[i] - output[i-1])
            zero_crossings_pos.append(i - 1 + t)

    # Expected positions: at mid-beat (0.5 * samples_per_beat from each beat start)
    expected_crossings = [(beat + 0.5) * samples_per_beat for beat in range(num_beats)]

    print(f"  SAW LFO @ 1 cycle/beat, {num_beats} beats:")
    print(f"    Positive zero crossings: {len(zero_crossings_pos)} (expected {num_beats})")

    # Calculate timing errors
    timing_errors = []
    for i, expected in enumerate(expected_crossings):
        if i < len(zero_crossings_pos):
            actual = zero_crossings_pos[i]
            error = actual - expected
            timing_errors.append(error)

    if timing_errors:
        max_error = max(abs(e) for e in timing_errors)
        mean_error = sum(timing_errors) / len(timing_errors)
        drift = timing_errors[-1] if timing_errors else 0

        passed = max_error <= 1.5  # Within 1.5 samples
        status = "PASS" if passed else "FAIL"

        results['tests'].append({
            'shape': 'SAW',
            'freq_mult': 1.0,
            'num_beats': num_beats,
            'zero_crossings_detected': len(zero_crossings_pos),
            'max_error_samples': float(max_error),
            'mean_error_samples': float(mean_error),
            'final_drift_samples': float(drift),
            'first_10_errors': timing_errors[:10],
            'last_10_errors': timing_errors[-10:] if len(timing_errors) >= 10 else timing_errors,
            'passed': passed
        })

        print(f"    Max error: {max_error:.3f} samples [{status}]")
        print(f"    Mean error: {mean_error:.3f} samples")
        print(f"    Final drift: {drift:.3f} samples")
        print(f"    First 5 errors: {[f'{e:.2f}' for e in timing_errors[:5]]}")
    else:
        print(f"    ERROR: No zero crossings detected!")
        results['tests'].append({
            'shape': 'SAW',
            'error': 'No zero crossings detected',
            'passed': False
        })

    # Also test phase wrapping (negative-going crossings at beat boundaries)
    print(f"\n  Testing phase wrap (beat boundaries)...")
    negative_crossings = []
    for i in range(1, len(output)):
        if output[i-1] > 0.5 and output[i] < -0.5:  # Large drop = phase wrap
            # This happens at beat boundaries
            negative_crossings.append(i)

    expected_wraps = [int(beat * samples_per_beat) for beat in range(1, num_beats)]
    wrap_errors = []
    for i, expected in enumerate(expected_wraps):
        if i < len(negative_crossings):
            actual = negative_crossings[i]
            error = actual - expected
            wrap_errors.append(error)

    if wrap_errors:
        max_wrap_error = max(abs(e) for e in wrap_errors)
        passed = max_wrap_error <= 1.5
        status = "PASS" if passed else "FAIL"

        results['tests'].append({
            'measurement': 'phase_wrap',
            'num_wraps': len(negative_crossings),
            'max_error_samples': float(max_wrap_error),
            'passed': passed
        })

        print(f"    Phase wraps detected: {len(negative_crossings)} (expected {num_beats - 1})")
        print(f"    Max wrap timing error: {max_wrap_error:.2f} samples [{status}]")

    with open(os.path.join(OUT, 'lfo_zero_crossing.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: {os.path.join(OUT, 'lfo_zero_crossing.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar LFO Opcode Tests")
    print("=" * 60)
    print()

    test_lfo_shapes()
    test_lfo_freq_sync()
    test_lfo_pwm()
    test_lfo_zero_crossing_precision()

    print()
    print("=" * 60)
    print("All LFO tests complete.")

"""
Clock Opcode Quality Tests (Cedar Engine)
==========================================
Tests for CLOCK opcode: phase accuracy and sample-level precision.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, save_wav
from visualize import save_figure

OUT = output_dir("op_clock")


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

    def create_clock_program(self, phase_type: int = 0, state_id: int = 1):
        """Create CLOCK program.

        Args:
            phase_type: 0=beat_phase, 1=bar_phase, 2=cycle_offset
            state_id: State ID
        """
        self.program = []

        # CLOCK outputs to buffer 10
        inst = cedar.Instruction.make_nullary(cedar.Opcode.CLOCK, 10, state_id)
        inst.rate = phase_type
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
# Test 1: CLOCK Phase Accuracy
# =============================================================================

def test_clock_phase():
    """Test CLOCK beat and bar phase accuracy."""
    print("Test 1: CLOCK Phase Accuracy")
    print("=" * 60)

    results = {'tests': []}

    # Test at various BPMs
    bpms = [60, 120, 180, 90]
    sr = 48000

    fig, axes = plt.subplots(len(bpms), 2, figsize=(16, 4 * len(bpms)))

    for idx, bpm in enumerate(bpms):
        samples_per_beat = sr * 60.0 / bpm
        samples_per_bar = samples_per_beat * 4

        # Test 4 bars
        duration = 4 * 4 * 60.0 / bpm

        # Test beat phase
        host = SequencerTestHost(sr, bpm)
        host.create_clock_program(phase_type=0, state_id=idx*10+1)  # beat_phase
        beat_output = host.run(duration)

        # Test bar phase
        host2 = SequencerTestHost(sr, bpm)
        host2.create_clock_program(phase_type=1, state_id=idx*10+2)  # bar_phase
        bar_output = host2.run(duration)

        # Analyze beat phase: should wrap at every beat
        beat_wraps = []
        for i in range(1, len(beat_output)):
            if beat_output[i] < beat_output[i-1] - 0.5:  # Phase wrapped
                beat_wraps.append(i)

        # Calculate beat timing accuracy
        expected_beats = int(duration * bpm / 60)
        actual_beats = len(beat_wraps)

        if len(beat_wraps) >= 2:
            beat_intervals = np.diff(beat_wraps)
            avg_beat_interval = np.mean(beat_intervals)
            beat_error = (avg_beat_interval - samples_per_beat) / samples_per_beat * 100
        else:
            avg_beat_interval = 0
            beat_error = 0

        # Analyze bar phase
        bar_wraps = []
        for i in range(1, len(bar_output)):
            if bar_output[i] < bar_output[i-1] - 0.5:
                bar_wraps.append(i)

        if len(bar_wraps) >= 2:
            bar_intervals = np.diff(bar_wraps)
            avg_bar_interval = np.mean(bar_intervals)
            bar_error = (avg_bar_interval - samples_per_bar) / samples_per_bar * 100
        else:
            avg_bar_interval = 0
            bar_error = 0

        test_result = {
            'bpm': bpm,
            'expected_samples_per_beat': samples_per_beat,
            'measured_samples_per_beat': float(avg_beat_interval),
            'beat_error_pct': float(beat_error),
            'expected_samples_per_bar': samples_per_bar,
            'measured_samples_per_bar': float(avg_bar_interval),
            'bar_error_pct': float(bar_error),
            'beats_detected': actual_beats,
            'expected_beats': expected_beats
        }
        results['tests'].append(test_result)

        beat_status = "PASS" if abs(beat_error) < 0.1 else "FAIL"
        bar_status = "PASS" if abs(bar_error) < 0.1 else "FAIL"

        print(f"\n  BPM={bpm}:")
        print(f"    Beat phase: expected={samples_per_beat:.1f} samples/beat, measured={avg_beat_interval:.1f}, error={beat_error:.3f}% [{beat_status}]")
        print(f"    Bar phase:  expected={samples_per_bar:.1f} samples/bar, measured={avg_bar_interval:.1f}, error={bar_error:.3f}% [{bar_status}]")
        print(f"    Beats: expected={expected_beats}, detected={actual_beats}")

        # Plot beat phase
        ax1 = axes[idx, 0]
        time_ms = np.arange(min(len(beat_output), int(2 * samples_per_bar))) / sr * 1000
        plot_samples = len(time_ms)
        ax1.plot(time_ms, beat_output[:plot_samples], 'b-', linewidth=0.8, label='Beat phase')

        # Mark beat boundaries
        for wrap in beat_wraps:
            if wrap < plot_samples:
                ax1.axvline(wrap / sr * 1000, color='red', linestyle=':', alpha=0.3)

        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Phase (0-1)')
        ax1.set_title(f'Beat Phase @ {bpm} BPM')
        ax1.legend()
        ax1.grid(True, alpha=0.3)
        ax1.set_ylim(-0.1, 1.1)

        # Plot bar phase
        ax2 = axes[idx, 1]
        ax2.plot(time_ms, bar_output[:plot_samples], 'g-', linewidth=0.8, label='Bar phase')

        # Mark bar boundaries
        for wrap in bar_wraps:
            if wrap < plot_samples:
                ax2.axvline(wrap / sr * 1000, color='red', linestyle=':', alpha=0.5)

        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Phase (0-1)')
        ax2.set_title(f'Bar Phase @ {bpm} BPM')
        ax2.legend()
        ax2.grid(True, alpha=0.3)
        ax2.set_ylim(-0.1, 1.1)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'clock.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'clock.png')}")

    with open(os.path.join(OUT, 'clock.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'clock.json')}")

    return results


# =============================================================================
# Test 2: CLOCK Phase Sample Accuracy
# =============================================================================

def test_clock_phase_sample_accuracy():
    """Test CLOCK phase accuracy at exact sample positions."""
    print("\nTest 2: CLOCK Phase Sample Accuracy")
    print("=" * 60)

    sr = 48000
    bpm = 120
    samples_per_beat = sr * 60.0 / bpm  # 24000 samples

    results = {'sample_rate': sr, 'bpm': bpm, 'samples_per_beat': samples_per_beat, 'tests': []}

    # Run for 100 beats
    num_beats = 100
    duration = num_beats * 60.0 / bpm

    host = SequencerTestHost(sr, bpm)
    host.create_clock_program(phase_type=0, state_id=1000)  # beat_phase
    output = host.run(duration)

    print(f"\n  Testing phase at exact sample positions over {num_beats} beats...")

    # Test phase values at specific samples
    test_samples = [
        0,                                    # Start
        int(samples_per_beat * 0.25),        # Quarter beat
        int(samples_per_beat * 0.5),         # Half beat
        int(samples_per_beat * 0.75),        # 3/4 beat
        int(samples_per_beat) - 1,           # Just before beat boundary
        int(samples_per_beat),               # Beat boundary
        int(samples_per_beat * 10),          # 10 beats in
        int(samples_per_beat * 50),          # 50 beats in
        int(samples_per_beat * 99),          # 99 beats in
    ]

    max_error = 0.0
    all_errors = []

    for sample_idx in test_samples:
        if sample_idx < len(output):
            # Expected phase: (sample_idx % samples_per_beat) / samples_per_beat
            expected_phase = (sample_idx % samples_per_beat) / samples_per_beat
            measured_phase = output[sample_idx]
            error = abs(measured_phase - expected_phase)
            max_error = max(max_error, error)
            all_errors.append(error)

            # Phase error as fraction of a sample
            phase_increment = 1.0 / samples_per_beat
            error_in_samples = error / phase_increment

            passed = error_in_samples < 2.0  # Within 2 sample equivalents
            status = "PASS" if passed else "FAIL"

            results['tests'].append({
                'sample_idx': sample_idx,
                'expected_phase': float(expected_phase),
                'measured_phase': float(measured_phase),
                'error': float(error),
                'error_in_samples': float(error_in_samples),
                'passed': passed
            })

            print(f"    Sample {sample_idx:8d}: expected={expected_phase:.6f}, "
                  f"measured={measured_phase:.6f}, error={error_in_samples:+.3f} samples [{status}]")

    # Long-term drift test: measure phase at every beat boundary
    print(f"\n  Long-term drift test over {num_beats} beats...")
    beat_boundary_errors = []

    for beat in range(num_beats):
        boundary_sample = int(beat * samples_per_beat)
        if boundary_sample < len(output):
            # At beat boundary, phase should wrap to near 0 (or 1.0 just before)
            measured = output[boundary_sample]
            # Phase should be very small (just wrapped) or very close to 0
            error = min(measured, 1.0 - measured)  # Distance to 0 or 1
            beat_boundary_errors.append(float(error))

    if beat_boundary_errors:
        max_boundary_error = max(beat_boundary_errors)
        avg_boundary_error = sum(beat_boundary_errors) / len(beat_boundary_errors)
        cumulative_drift = beat_boundary_errors[-1] if beat_boundary_errors else 0

        # Convert to samples
        phase_increment = 1.0 / samples_per_beat
        max_error_samples = max_boundary_error / phase_increment
        avg_error_samples = avg_boundary_error / phase_increment

        passed = max_error_samples < 2.0
        status = "PASS" if passed else "FAIL"

        results['long_term'] = {
            'num_beats': num_beats,
            'max_boundary_error': max_boundary_error,
            'max_error_samples': max_error_samples,
            'avg_error_samples': avg_error_samples,
            'passed': passed
        }

        print(f"    Max error at beat boundaries: {max_error_samples:.3f} samples [{status}]")
        print(f"    Average error: {avg_error_samples:.3f} samples")

    with open(os.path.join(OUT, 'clock_sample_accuracy.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: {os.path.join(OUT, 'clock_sample_accuracy.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar Clock Opcode Tests")
    print("=" * 60)
    print()

    test_clock_phase()
    test_clock_phase_sample_accuracy()

    print()
    print("=" * 60)
    print("All clock tests complete.")

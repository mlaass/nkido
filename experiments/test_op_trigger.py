"""
Trigger Opcode Quality Tests (Cedar Engine)
============================================
Tests for TRIGGER opcode: division accuracy, audio integration,
and long-term precision.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, save_wav
from visualize import save_figure

OUT = output_dir("op_trigger")


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

    def create_trigger_program(self, division: float, state_id: int = 1):
        """Create TRIGGER program.

        Args:
            division: Triggers per beat (1=quarter, 2=eighth, 4=16th)
            state_id: State ID
        """
        self.program = []

        self.vm.set_param("division", division)

        # Get division into buffer 1
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("division"))
        )

        # TRIGGER: division (buf 1) -> output (buf 10)
        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.TRIGGER, 10, 1, state_id)
        )

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


def count_triggers(signal: np.ndarray, threshold: float = 0.5, include_initial: bool = True) -> list:
    """Count rising edges (triggers) and return their positions.

    Args:
        signal: Input signal
        threshold: Trigger threshold
        include_initial: If True, include a trigger at sample 0 if signal starts above threshold

    Returns:
        List of sample indices where triggers occur
    """
    triggers = []
    # Check sample 0 as a special case (trigger at start)
    if include_initial and len(signal) > 0 and signal[0] >= threshold:
        triggers.append(0)
    # Check for rising edges from sample 1 onwards
    for i in range(1, len(signal)):
        if signal[i-1] < threshold and signal[i] >= threshold:
            triggers.append(i)
    return triggers


def analyze_trigger_timing(triggers: list, expected_interval: float, sr: int) -> dict:
    """Analyze trigger timing accuracy.

    Args:
        triggers: List of trigger sample indices
        expected_interval: Expected samples between triggers
        sr: Sample rate

    Returns:
        Dict with timing analysis
    """
    if len(triggers) < 2:
        return {'error': 'Not enough triggers'}

    intervals = np.diff(triggers)
    avg_interval = np.mean(intervals)
    interval_error = (avg_interval - expected_interval) / expected_interval * 100
    jitter = np.std(intervals)

    return {
        'num_triggers': len(triggers),
        'avg_interval_samples': float(avg_interval),
        'expected_interval_samples': expected_interval,
        'interval_error_pct': float(interval_error),
        'jitter_samples': float(jitter),
        'jitter_ms': float(jitter / sr * 1000),
        'intervals': intervals.tolist()[:20]  # First 20 intervals
    }


# =============================================================================
# Test 1: TRIGGER Division Accuracy
# =============================================================================

def test_trigger_division():
    """Test TRIGGER beat division accuracy."""
    print("Test 1: TRIGGER Division Accuracy")
    print("=" * 60)

    sr = 48000
    bpm = 120

    divisions = [1, 2, 4, 8, 16]

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    fig, axes = plt.subplots(len(divisions), 1, figsize=(14, 2 * len(divisions)))

    for idx, division in enumerate(divisions):
        host = SequencerTestHost(sr, bpm)
        host.create_trigger_program(float(division), state_id=idx+400)

        # 4 beats
        duration = 4 * 60.0 / bpm
        output = host.run(duration)

        # Find triggers
        triggers = count_triggers(output, threshold=0.5)

        expected_triggers = 4 * division  # 4 beats * division per beat
        expected_interval = host.samples_per_beat / division

        timing = analyze_trigger_timing(triggers, expected_interval, sr)

        test_result = {
            'division': division,
            'expected_triggers': expected_triggers,
            'actual_triggers': len(triggers),
            **timing
        }
        results['tests'].append(test_result)

        trigger_status = "PASS" if abs(len(triggers) - expected_triggers) <= 2 else "FAIL"
        timing_status = "PASS" if abs(timing.get('interval_error_pct', 100)) < 1 else "FAIL"

        print(f"  div={division:2d}: expected={expected_triggers:3d} triggers, actual={len(triggers):3d} [{trigger_status}], "
              f"interval_error={timing.get('interval_error_pct', 0):.2f}% [{timing_status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)

        # Mark triggers
        for trig in triggers[:50]:  # First 50
            ax.axvline(trig / host.samples_per_beat, color='red', linestyle=':', alpha=0.3)

        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Trigger')
        ax.set_title(f'TRIGGER div={division} ({len(triggers)} triggers in 4 beats)')
        ax.set_xlim(0, 4)
        ax.set_ylim(-0.1, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'trigger.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'trigger.png')}")

    with open(os.path.join(OUT, 'trigger.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'trigger.json')}")

    return results


# =============================================================================
# Test 2: Trigger to Audio (Integration)
# =============================================================================

def test_trigger_audio():
    """Test trigger-to-audio integration using envelopes."""
    print("\nTest 2: Trigger to Audio Integration")
    print("=" * 60)

    sr = 48000
    bpm = 120

    # Create a simple kick drum patch using TRIGGER -> ENV_AR -> OSC

    # First, generate triggers
    host = SequencerTestHost(sr, bpm)

    # Program:
    # 1. TRIGGER div=1 -> buf 5 (quarter notes)
    # 2. ENV_AR (trigger=buf5, attack=0.001, release=0.05) -> buf 6
    # 3. OSC_SIN (freq=60) -> buf 7
    # 4. MUL buf6 * buf7 -> buf 10
    # 5. OUTPUT buf 10

    host.program = []

    # Set parameters
    host.vm.set_param("division", 1.0)
    host.vm.set_param("attack", 0.001)
    host.vm.set_param("release", 0.1)
    host.vm.set_param("freq", 60.0)

    # Get division -> buf 1
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("division"))
    )
    # Get attack -> buf 2
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("attack"))
    )
    # Get release -> buf 3
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 3, cedar.hash("release"))
    )
    # Get freq -> buf 4
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 4, cedar.hash("freq"))
    )

    # TRIGGER -> buf 5
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.TRIGGER, 5, 1, cedar.hash("kick_trig"))
    )

    # ENV_AR -> buf 6
    host.program.append(
        cedar.Instruction.make_ternary(cedar.Opcode.ENV_AR, 6, 5, 2, 3, cedar.hash("kick_env"))
    )

    # OSC_SIN -> buf 7
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_SIN, 7, 4, cedar.hash("kick_osc"))
    )

    # MUL buf6 * buf7 -> buf 10
    host.program.append(
        cedar.Instruction.make_binary(cedar.Opcode.MUL, 10, 6, 7, 0)
    )

    # OUTPUT
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
    )

    host.vm.load_program(host.program)

    # Run for 4 bars
    duration = 16 * 60.0 / bpm
    output = host.run(duration)

    # Analyze output
    peak_level = np.max(np.abs(output))
    rms_level = np.sqrt(np.mean(output**2))

    # Find peaks (kick hits)
    peaks = []
    threshold = peak_level * 0.5
    in_peak = False
    for i, val in enumerate(output):
        if abs(val) > threshold and not in_peak:
            peaks.append(i)
            in_peak = True
        elif abs(val) < threshold * 0.5:
            in_peak = False

    print(f"  Generated kick drum pattern:")
    print(f"    Peak level: {peak_level:.3f}")
    print(f"    RMS level: {rms_level:.4f}")
    print(f"    Kicks detected: {len(peaks)} (expected 16)")

    # Save results
    results = {
        'sample_rate': sr,
        'bpm': bpm,
        'duration_sec': duration,
        'peak_level': float(peak_level),
        'rms_level': float(rms_level),
        'kicks_detected': len(peaks),
        'expected_kicks': 16
    }

    with open(os.path.join(OUT, 'trigger_audio.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: {os.path.join(OUT, 'trigger_audio.json')}")

    # Plot
    fig, axes = plt.subplots(2, 1, figsize=(14, 8))

    # Full waveform
    ax1 = axes[0]
    time_beats = np.arange(len(output)) / host.samples_per_beat
    ax1.plot(time_beats, output, linewidth=0.5)
    ax1.set_xlabel('Time (beats)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title(f'Kick Drum Pattern (quarter notes @ {bpm} BPM)')
    ax1.grid(True, alpha=0.3)

    # Zoomed view (first 4 beats)
    ax2 = axes[1]
    zoom_samples = int(4 * host.samples_per_beat)
    time_ms = np.arange(zoom_samples) / sr * 1000
    ax2.plot(time_ms, output[:zoom_samples], linewidth=0.8)
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Amplitude')
    ax2.set_title('First 4 beats (zoomed)')
    ax2.grid(True, alpha=0.3)

    # Mark beat boundaries
    for beat in range(5):
        ax2.axvline(beat * 60000 / bpm, color='red', linestyle='--', alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'trigger_audio.png'))
    print(f"  Saved: {os.path.join(OUT, 'trigger_audio.png')}")

    # Save audio
    save_wav(os.path.join(OUT, 'kick_pattern.wav'), output, sr)

    return results


# =============================================================================
# Test 3: TRIGGER Long-Term Precision (1000 beats)
# =============================================================================

def test_trigger_long_term_precision():
    """Test TRIGGER timing precision over 1000 beats."""
    print("\nTest 3: TRIGGER Long-Term Precision")
    print("=" * 60)

    sr = 48000
    bpm = 120
    num_beats = 1000

    results = {'sample_rate': sr, 'bpm': bpm, 'num_beats': num_beats, 'tests': []}

    divisions = [1, 2, 4, 8]

    for division in divisions:
        samples_per_trigger = sr * 60.0 / bpm / division
        expected_triggers = num_beats * division
        duration = num_beats * 60.0 / bpm

        print(f"\n  Division={division} ({expected_triggers} triggers expected over {num_beats} beats)...")

        host = SequencerTestHost(sr, bpm)
        host.create_trigger_program(float(division), state_id=1100 + division)
        output = host.run(duration)

        # Find ALL trigger positions
        triggers = count_triggers(output, threshold=0.5)

        # Calculate timing error for EVERY trigger
        errors_samples = []
        for i, trigger_sample in enumerate(triggers):
            expected_sample = i * samples_per_trigger
            error = trigger_sample - expected_sample
            errors_samples.append(error)

        if errors_samples:
            max_error = max(abs(e) for e in errors_samples)
            final_drift = errors_samples[-1] if errors_samples else 0
            mean_error = sum(errors_samples) / len(errors_samples)

            # Pass criteria: <=1 sample error at any point
            passed = max_error <= 1.5
            status = "PASS" if passed else "FAIL"

            test_result = {
                'division': division,
                'expected_triggers': expected_triggers,
                'actual_triggers': len(triggers),
                'max_error_samples': float(max_error),
                'final_drift_samples': float(final_drift),
                'mean_error_samples': float(mean_error),
                'passed': passed,
                # Store first and last 10 errors for inspection
                'first_10_errors': errors_samples[:10],
                'last_10_errors': errors_samples[-10:] if len(errors_samples) >= 10 else errors_samples
            }
            results['tests'].append(test_result)

            trigger_count_status = "PASS" if abs(len(triggers) - expected_triggers) <= 2 else "FAIL"
            print(f"    Triggers: expected={expected_triggers}, actual={len(triggers)} [{trigger_count_status}]")
            print(f"    Max timing error: {max_error:.2f} samples [{status}]")
            print(f"    Final drift: {final_drift:.2f} samples")
            print(f"    Mean error: {mean_error:.2f} samples")
        else:
            print(f"    ERROR: No triggers detected!")
            results['tests'].append({
                'division': division,
                'expected_triggers': expected_triggers,
                'actual_triggers': 0,
                'error': 'No triggers detected',
                'passed': False
            })

    # Summary
    all_passed = all(t.get('passed', False) for t in results['tests'])
    summary = "ALL PASSED" if all_passed else "SOME FAILED"
    print(f"\n  Overall: {summary}")

    with open(os.path.join(OUT, 'trigger_long_term.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'trigger_long_term.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar Trigger Opcode Tests")
    print("=" * 60)
    print()

    test_trigger_division()
    test_trigger_audio()
    test_trigger_long_term_precision()

    print()
    print("=" * 60)
    print("All trigger tests complete.")

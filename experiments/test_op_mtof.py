"""
MTOF Opcode Quality Test (Cedar Engine)
========================================
Tests MIDI to frequency conversion accuracy across the full MIDI range.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, midi_to_freq
from visualize import save_figure

OUT = output_dir("op_mtof")


# =============================================================================
# MTOF Test - MIDI to Frequency Accuracy
# =============================================================================

def test_mtof_accuracy():
    """
    Test MIDI to frequency conversion accuracy.
    - Standard A4=69 -> 440Hz
    - Full MIDI range 0-127
    - Tolerance: <0.1% frequency error
    """
    print("\nTest 2: MTOF (MIDI to Frequency) Accuracy")
    print("=" * 60)

    sr = 48000

    # Reference test cases
    reference_notes = [
        (69, 440.0, "A4"),      # A440
        (60, 261.626, "C4"),    # Middle C
        (57, 220.0, "A3"),      # A3
        (81, 880.0, "A5"),      # A5
        (48, 130.813, "C3"),    # C3
        (36, 65.406, "C2"),     # C2
        (84, 1046.50, "C6"),    # C6
        (21, 27.5, "A0"),       # A0 (low piano)
        (108, 4186.01, "C8"),   # C8 (high piano)
    ]

    results = {'sample_rate': sr, 'tests': [], 'full_range': []}

    print("\n  Reference Note Tests:")

    all_passed = True
    for midi_note, expected_freq, name in reference_notes:
        host = CedarTestHost(sr)

        # Set MIDI note as input
        buf_midi = host.set_param("midi", float(midi_note))
        buf_out = 1

        # MTOF: out = mtof(midi_note)
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.MTOF, buf_out, buf_midi, cedar.hash("mtof") & 0xFFFF)
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Process one block
        silence = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
        output = host.process(silence)

        measured_freq = float(output[0])
        error_pct = abs(measured_freq - expected_freq) / expected_freq * 100
        passed = error_pct < 0.1

        if not passed:
            all_passed = False

        results['tests'].append({
            'name': name,
            'midi_note': midi_note,
            'expected_hz': expected_freq,
            'measured_hz': measured_freq,
            'error_pct': error_pct,
            'passed': passed
        })

        status = "PASS" if passed else "FAIL"
        print(f"    {name:4s} (MIDI {midi_note:3d}): expected={expected_freq:8.2f}Hz, "
              f"measured={measured_freq:8.2f}Hz, error={error_pct:.4f}% [{status}]")

    # Full range test
    print("\n  Full MIDI Range Test (0-127):")
    max_error = 0
    errors = []

    for midi_note in range(128):
        expected_freq = 440.0 * (2 ** ((midi_note - 69) / 12))

        host = CedarTestHost(sr)
        buf_midi = host.set_param("midi", float(midi_note))
        buf_out = 1

        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.MTOF, buf_out, buf_midi, cedar.hash("mtof") & 0xFFFF)
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        silence = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
        output = host.process(silence)
        measured_freq = float(output[0])

        error_pct = abs(measured_freq - expected_freq) / expected_freq * 100
        errors.append(error_pct)
        max_error = max(max_error, error_pct)

        results['full_range'].append({
            'midi_note': midi_note,
            'expected_hz': expected_freq,
            'measured_hz': measured_freq,
            'error_pct': error_pct
        })

    avg_error = np.mean(errors)
    range_passed = max_error < 0.1

    print(f"    Max error: {max_error:.6f}%")
    print(f"    Avg error: {avg_error:.6f}%")
    print(f"    Range test: {'PASS' if range_passed else 'FAIL'}")

    results['summary'] = {
        'max_error_pct': max_error,
        'avg_error_pct': avg_error,
        'all_reference_passed': all_passed,
        'full_range_passed': range_passed
    }

    # Visualization
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # MIDI to Frequency curve
    ax1 = axes[0, 0]
    midi_notes = np.arange(128)
    measured_freqs = [r['measured_hz'] for r in results['full_range']]
    expected_freqs = [r['expected_hz'] for r in results['full_range']]
    ax1.semilogy(midi_notes, expected_freqs, 'b-', linewidth=2, label='Expected')
    ax1.semilogy(midi_notes, measured_freqs, 'r--', linewidth=1, label='Measured')
    ax1.set_xlabel('MIDI Note')
    ax1.set_ylabel('Frequency (Hz)')
    ax1.set_title('MIDI to Frequency Conversion')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Error across range
    ax2 = axes[0, 1]
    ax2.plot(midi_notes, errors, 'b-', linewidth=1)
    ax2.axhline(0.1, color='red', linestyle='--', alpha=0.5, label='0.1% threshold')
    ax2.set_xlabel('MIDI Note')
    ax2.set_ylabel('Error (%)')
    ax2.set_title('Conversion Error Across MIDI Range')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(0, max(0.2, max_error * 1.1))

    # Reference notes bar chart
    ax3 = axes[1, 0]
    ref_names = [r['name'] for r in results['tests']]
    ref_errors = [r['error_pct'] for r in results['tests']]
    colors = ['green' if e < 0.1 else 'red' for e in ref_errors]
    ax3.bar(ref_names, ref_errors, color=colors)
    ax3.axhline(0.1, color='red', linestyle='--', alpha=0.5)
    ax3.set_xlabel('Note')
    ax3.set_ylabel('Error (%)')
    ax3.set_title('Reference Note Errors')
    ax3.grid(True, alpha=0.3)

    # Error histogram
    ax4 = axes[1, 1]
    ax4.hist(errors, bins=50, edgecolor='black', alpha=0.7)
    ax4.axvline(0.1, color='red', linestyle='--', alpha=0.7, label='0.1% threshold')
    ax4.set_xlabel('Error (%)')
    ax4.set_ylabel('Count')
    ax4.set_title('Error Distribution')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'accuracy.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'accuracy.png')}")

    with open(os.path.join(OUT, 'accuracy.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'accuracy.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar MTOF Opcode Quality Test")
    print("=" * 60)
    print()

    test_mtof_accuracy()

    print()
    print("=" * 60)
    print("MTOF test complete.")

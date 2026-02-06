"""
Test: TIMELINE (Breakpoint Automation)
=======================================
Tests timeline breakpoint interpolation, hold mode, and loop wrapping.

NOTE: TimelineState is populated by the Akkado compiler — the Python bindings
do not expose an API to set up breakpoints directly. This test verifies basic
opcode behavior (zero-breakpoint fallback) and documents expected behavior.

Expected behavior:
- With no breakpoints: outputs zero
- Linear interpolation between breakpoints (curve=0)
- Hold mode (curve=2): value stays constant until next breakpoint
- Loop wrapping: automation repeats at loop_length

If this test fails, check the implementation in cedar/include/cedar/opcodes/sequencing.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import output_dir
from visualize import save_figure

OUT = output_dir("op_timeline")


def test_zero_breakpoints():
    """
    Test TIMELINE with no breakpoints produces zero output.

    Expected:
    - Output should be zero
    - No crashes
    """
    print("Test: TIMELINE Zero Breakpoints")

    sr = 48000
    duration = 0.5
    bpm = 120.0

    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_bpm(bpm)

    state_id = cedar.hash("timeline_test")
    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.TIMELINE, 1, state_id),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    all_zero = True

    for _ in range(num_blocks):
        left, right = vm.process()
        if np.any(np.abs(left) > 1e-10):
            all_zero = False
            break

    if all_zero:
        print("  ✓ PASS: Zero breakpoints produces silence")
    else:
        print("  ✗ FAIL: Non-zero output with zero breakpoints")


def test_no_crash_long_run():
    """
    Test TIMELINE doesn't crash over extended processing.

    Expected:
    - Stable processing over many blocks
    """
    print("Test: TIMELINE Stability (long run)")

    sr = 48000
    duration = 5.0
    bpm = 120.0

    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_bpm(bpm)

    state_id = cedar.hash("timeline_stable")
    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.TIMELINE, 1, state_id),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    try:
        for _ in range(num_blocks):
            vm.process()
        print(f"  ✓ PASS: Processed {num_blocks} blocks without crash")
    except Exception as e:
        print(f"  ✗ FAIL: Crashed: {e}")


def test_documentation():
    """
    Document expected behavior for when state population bindings are added.
    """
    print("Test: TIMELINE Expected Behavior Documentation")
    print("  NOTE: Full testing requires Python bindings to populate TimelineState")
    print("  Expected behavior when breakpoints are populated:")
    print("    - Linear (curve=0): smooth interpolation between points")
    print("    - Exponential (curve=1): curved transitions")
    print("    - Hold (curve=2): value stays constant until next breakpoint")
    print("    - Loop: automation repeats after loop_length beats")
    print("  ⚠ SKIP: Cannot populate TimelineState from Python bindings")

    # Create visualization of expected behavior
    fig, axes = plt.subplots(3, 1, figsize=(12, 9))
    fig.suptitle("TIMELINE Expected Behavior")

    beats = np.linspace(0, 4, 1000)

    # Linear interpolation
    breakpoints = [(0.0, 0.0), (1.0, 1.0), (2.0, 0.3), (3.0, 0.8), (4.0, 0.0)]
    ax = axes[0]
    bp_beats = [b[0] for b in breakpoints]
    bp_values = [b[1] for b in breakpoints]
    ax.plot(bp_beats, bp_values, 'b-', linewidth=2, label='Linear (curve=0)')
    ax.plot(bp_beats, bp_values, 'ro', markersize=8)
    ax.set_title('Linear Interpolation')
    ax.set_xlabel('Beat')
    ax.set_ylabel('Value')
    ax.set_xlim(0, 4)
    ax.set_ylim(-0.1, 1.1)
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Hold mode
    ax = axes[1]
    hold_beats = []
    hold_values = []
    for i in range(len(breakpoints) - 1):
        hold_beats.extend([breakpoints[i][0], breakpoints[i + 1][0]])
        hold_values.extend([breakpoints[i][1], breakpoints[i][1]])
    ax.plot(hold_beats, hold_values, 'g-', linewidth=2, label='Hold (curve=2)')
    ax.plot(bp_beats, bp_values, 'ro', markersize=8)
    ax.set_title('Hold Mode')
    ax.set_xlabel('Beat')
    ax.set_ylabel('Value')
    ax.set_xlim(0, 4)
    ax.set_ylim(-0.1, 1.1)
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Loop wrapping
    ax = axes[2]
    loop_length = 2.0
    for loop in range(4):
        offset = loop * loop_length
        ax.plot([b + offset for b in bp_beats[:3]], bp_values[:3], 'b-', linewidth=2)
        ax.plot([b + offset for b in bp_beats[:3]], bp_values[:3], 'ro', markersize=6)
    ax.axvline(2, color='red', linestyle='--', alpha=0.5, label='Loop boundary')
    ax.axvline(4, color='red', linestyle='--', alpha=0.5)
    ax.axvline(6, color='red', linestyle='--', alpha=0.5)
    ax.set_title('Loop Wrapping (loop_length=2 beats)')
    ax.set_xlabel('Beat')
    ax.set_ylabel('Value')
    ax.set_xlim(0, 8)
    ax.set_ylim(-0.1, 1.1)
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "timeline_expected.png"))
    print(f"  Saved {os.path.join(OUT, 'timeline_expected.png')}")


if __name__ == "__main__":
    test_zero_breakpoints()
    test_no_crash_long_run()
    test_documentation()

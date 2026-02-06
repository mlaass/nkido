"""
Test: SEQ_STEP (Step Sequencer)
================================
Tests step sequencer event timing, value output, and cycle wrapping.

NOTE: SeqStepState is populated by the Akkado compiler — the Python bindings
do not expose an API to set up events directly. This test verifies basic
opcode behavior (zero-event fallback) and documents expected behavior for
when bindings are extended.

Expected behavior:
- With no events: outputs silence (value=0, velocity=0, trigger=0)
- With events populated: triggers fire at correct beat positions
- Values held between events
- Cycle wrapping works correctly

If this test fails, check the implementation in cedar/include/cedar/opcodes/sequencing.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from visualize import save_figure

OUT = output_dir("op_seq_step")


def test_zero_events():
    """
    Test SEQ_STEP with no events produces silence.

    Expected:
    - All output buffers (value, velocity, trigger) should be zero
    - No crashes or undefined behavior
    """
    print("Test: SEQ_STEP Zero Events")

    sr = 48000
    duration = 0.5
    bpm = 120.0

    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_bpm(bpm)

    # SEQ_STEP: out=value_buf, inputs[0]=velocity_buf, inputs[1]=trigger_buf
    # With no events populated in state, it should output zeros
    state_id = cedar.hash("seq_test")
    program = [
        cedar.Instruction.make_ternary(
            cedar.Opcode.SEQ_STEP, 1, 2, 3, 0, state_id  # value→1, vel→2, trig→3
        ),
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
        print("  ✓ PASS: Zero events produces silence")
    else:
        print("  ✗ FAIL: Non-zero output with zero events")


def test_no_crash_long_run():
    """
    Test SEQ_STEP doesn't crash over extended processing.

    Expected:
    - Stable over many blocks (simulating live performance)
    - No memory errors or undefined behavior
    """
    print("Test: SEQ_STEP Stability (long run)")

    sr = 48000
    duration = 5.0  # 5 seconds
    bpm = 120.0

    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_bpm(bpm)

    state_id = cedar.hash("seq_stable")
    program = [
        cedar.Instruction.make_ternary(
            cedar.Opcode.SEQ_STEP, 1, 2, 3, 0, state_id
        ),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    try:
        for _ in range(num_blocks):
            vm.process()
        print(f"  ✓ PASS: Processed {num_blocks} blocks without crash")
    except Exception as e:
        print(f"  ✗ FAIL: Crashed after processing: {e}")


def test_documentation():
    """
    Document expected behavior for when state population bindings are added.
    """
    print("Test: SEQ_STEP Expected Behavior Documentation")
    print("  NOTE: Full testing requires Python bindings to populate SeqStepState")
    print("  Expected behavior when events are populated:")
    print("    - Events at times [0.0, 1.0, 2.0, 3.0] with cycle_length=4.0")
    print("    - At 120 BPM: triggers every 0.5s (24000 samples)")
    print("    - Value output: held constant between events")
    print("    - Velocity output: per-event velocity (0.0-1.0)")
    print("    - Trigger output: 1.0 at exact beat position, 0.0 otherwise")
    print("    - Cycle wrapping: events repeat after cycle_length beats")
    print("  ⚠ SKIP: Cannot populate SeqStepState from Python bindings")

    # Create a placeholder plot documenting the expected behavior
    fig, ax = plt.subplots(figsize=(12, 4))

    # Draw expected output pattern
    beats = np.linspace(0, 8, 1000)
    expected_trigger = np.zeros_like(beats)
    expected_value = np.zeros_like(beats)
    values = [0.5, 0.7, 0.3, 0.9]

    for i in range(8):
        trigger_pos = i
        idx = np.argmin(np.abs(beats - trigger_pos))
        expected_trigger[idx] = 1.0
        # Hold value until next event
        cycle_idx = i % 4
        start = idx
        end = np.argmin(np.abs(beats - (trigger_pos + 1))) if i < 7 else len(beats)
        expected_value[start:end] = values[cycle_idx]

    ax.step(beats, expected_value, 'b-', linewidth=1.5, label='Value (expected)', where='post')
    for i in range(8):
        ax.axvline(i, color='red', linestyle=':', alpha=0.5)
        ax.plot(i, 1.0, 'rv', markersize=8)
    ax.set_xlabel('Beat Position')
    ax.set_ylabel('Value')
    ax.set_title('SEQ_STEP Expected Behavior (4-event cycle at 120 BPM)')
    ax.set_xlim(0, 8)
    ax.set_ylim(-0.1, 1.1)
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "seq_step_expected.png"))
    print(f"  Saved {os.path.join(OUT, 'seq_step_expected.png')}")


if __name__ == "__main__":
    test_zero_events()
    test_no_crash_long_run()
    test_documentation()

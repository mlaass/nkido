"""
Step Pattern Diagnostic (Cedar Engine)
======================================
Reproduces the *forward stepper* shape from `web/static/patches/stepper-demo.akk`
at the Cedar VM level, with no Akkado compiler involved.

The Akkado source is:

    step = (arr, trig) -> arr[counter(trig)]
    notes = [57, 60, 64, 67, 72]
    melody = notes.step(trigger(4))

That should lower to roughly the program built below: PUSH_CONST × 5 +
ARRAY_PACK to make the array, EDGE_OP rate=3 (counter) to advance the index
on every rising edge of an external trigger, and ARRAY_INDEX (wrap mode) to
pick the note.

User report (2026-04-26): the demo plays only a static drone — the melody
voice does not walk through the notes. This test isolates whether the bug
is in the underlying Cedar opcodes (counter / array_pack / array_index)
or in Akkado lowering. If THIS passes, the bug is in lowering, not the VM.

Per CLAUDE.md sequenced-opcode rule, the long-run check covers ≥ 300s of
simulated audio.
"""

import os
import struct
import numpy as np
import scipy.io.wavfile
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir


OUT = output_dir("op_step_pattern")
SR = 48000
NOTES = [57.0, 60.0, 64.0, 67.0, 72.0]
LEN = float(len(NOTES))


def _f32_to_u32(x: float) -> int:
    """Bit-cast a float to its uint32 representation (for PUSH_CONST.state_id)."""
    return struct.unpack("<I", struct.pack("<f", x))[0]


def _push_const(buf: int, value: float) -> cedar.Instruction:
    inst = cedar.Instruction.make_nullary(cedar.Opcode.PUSH_CONST, buf)
    inst.state_id = _f32_to_u32(value)
    return inst


def _build_program():
    """
    Build a program equivalent to `arr[counter(trig)]` where arr = NOTES.

    Buffer map:
        0  external trigger input (set per block)
        1  array element 0 = 57
        2  array element 1 = 60
        3  array element 2 = 64
        4  array element 3 = 67
        5  array element 4 = 72
        6  packed array (5 floats)
        7  array length (constant 5)
        8  counter output (index)
        9  ARRAY_INDEX output (the note value)

    Then OUTPUT writes buffer 9 to the left output channel.
    """
    program = []
    # PUSH_CONST for each note value
    for i, v in enumerate(NOTES):
        program.append(_push_const(1 + i, v))
    # Pack 5 element buffers into one 5-slot array buffer
    pack = cedar.Instruction.make_quinary(
        cedar.Opcode.ARRAY_PACK, 6, 1, 2, 3, 4, 5
    )
    pack.rate = len(NOTES)
    program.append(pack)
    # Array length
    program.append(_push_const(7, LEN))
    # counter(trig) — EDGE_OP rate=3, single trig input, no reset/start
    counter = cedar.Instruction.make_ternary(
        cedar.Opcode.EDGE_OP, 8, 0, 0xFFFF, 0xFFFF, cedar.hash("step_test_counter")
    )
    counter.rate = 3
    program.append(counter)
    # ARRAY_INDEX wrap mode (rate=0): in0=arr, in1=idx, in2=length
    aidx = cedar.Instruction.make_ternary(cedar.Opcode.ARRAY_INDEX, 9, 6, 8, 7)
    aidx.rate = 0
    program.append(aidx)
    # OUTPUT
    program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 9))
    return program


def _trigger_block(block_idx: int, trig_period_samples: int) -> np.ndarray:
    """A single-sample rising edge wherever `global_sample % trig_period == 0`."""
    block = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
    base = block_idx * cedar.BLOCK_SIZE
    for i in range(cedar.BLOCK_SIZE):
        s = base + i
        # Skip s=0 to avoid initial-state ambiguity for the counter
        if s > 0 and s % trig_period_samples == 0:
            block[i] = 1.0
    return block


def test_short_walk_with_wav():
    """
    Short test (≈ 5 seconds) so the WAV is auditioning-friendly. Drives 5 Hz
    triggers and records the note value the program outputs after each one.
    Expected: the sequence of note-values on each trigger equals the cyclic
    walk through NOTES.
    """
    print("\n[short] forward stepper, ~5s @ 5 Hz triggers, with WAV")
    print("-" * 60)

    host = CedarTestHost(SR)
    host.program = _build_program()
    host.vm.load_program(host.program)

    duration_s = 5
    n_blocks = duration_s * SR // cedar.BLOCK_SIZE
    trig_period = SR // 5  # 5 Hz
    out_samples = []
    note_at_trigger = []
    last_value = None

    for block in range(n_blocks):
        trig = _trigger_block(block, trig_period)
        host.vm.set_buffer(0, trig)
        l, _ = host.vm.process()
        # Whenever the trigger fires anywhere in this block, record the value
        # *after* the trigger sample (so we capture the new index).
        for i in range(cedar.BLOCK_SIZE):
            if trig[i] > 0.0:
                # Look at the very next sample (or last sample of next block)
                # — the counter increments on rising edge, ARRAY_INDEX picks
                # the new note in the same sample.
                note_at_trigger.append(float(l[i]))
        out_samples.append(l)

    out = np.concatenate(out_samples)
    wav_path = os.path.join(OUT, "step_pattern.wav")
    # Output is MIDI note numbers, normalize to ±1 for an audible WAV
    norm = out / 127.0
    scipy.io.wavfile.write(wav_path, SR, norm.astype(np.float32))
    print(f"  Saved {wav_path} - listen for stair-step DC walking 5 levels")

    # Build expected walk: index 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, ...
    # (counter starts at 0 by EDGE_OP convention, then increments on the FIRST
    # rising edge to 1, so the first observed note is NOTES[1] not NOTES[0].)
    expected = [NOTES[(k + 1) % len(NOTES)] for k in range(len(note_at_trigger))]

    # Compare element-wise
    mismatches = [
        (i, note_at_trigger[i], expected[i])
        for i in range(len(note_at_trigger))
        if abs(note_at_trigger[i] - expected[i]) > 1e-3
    ]

    print(f"  observed first 12 notes: {[round(x,1) for x in note_at_trigger[:12]]}")
    print(f"  expected first 12 notes: {[round(x,1) for x in expected[:12]]}")
    if not mismatches:
        print(f"  ✓ PASS: forward stepper walks NOTES cyclically across {len(note_at_trigger)} triggers")
        return True
    else:
        print(f"  ✗ FAIL: {len(mismatches)} mismatches")
        for i, got, want in mismatches[:5]:
            print(f"     trigger #{i}: got {got}, want {want}")
        return False


def test_long_run_walks_arbitrary_count():
    """
    Long run (≥ 300s) at 4 Hz triggers (matches the demo's `trigger(4)` cadence
    if 1 cycle = 1s; the actual relation is irrelevant — we just need many
    triggers to surface long-tail bugs in counter/index wrap).

    Asserts the counter advances `expected_pulses` times and the final note
    output equals NOTES[expected_pulses % len(NOTES)].
    """
    print("\n[long] forward stepper, 300s @ 4 Hz triggers")
    print("-" * 60)

    host = CedarTestHost(SR)
    host.program = _build_program()
    host.vm.load_program(host.program)

    duration_s = 300
    n_blocks = duration_s * SR // cedar.BLOCK_SIZE
    trig_period = SR // 4  # 4 Hz
    pulses = 0
    last_value = 0.0

    for block in range(n_blocks):
        trig = _trigger_block(block, trig_period)
        pulses += int(np.sum(trig > 0.5))
        host.vm.set_buffer(0, trig)
        l, _ = host.vm.process()
        last_value = float(l[-1])

    # The counter starts at 0 and increments on each pulse, so after N pulses
    # the index is N. ARRAY_INDEX wrap → arr[N % len].
    expected_note = NOTES[pulses % len(NOTES)]
    ok = abs(last_value - expected_note) < 1e-3
    sym = "✓" if ok else "✗"
    print(f"  {sym} after {duration_s}s ({pulses} pulses): out={last_value:.2f}, expected NOTES[{pulses}%{len(NOTES)}]={expected_note}")
    if not ok:
        print(f"  ✗ FAIL: investigate counter (edge_op.hpp rate=3) or ARRAY_INDEX wrap")
    return ok


if __name__ == "__main__":
    print("Cedar Step-Pattern Diagnostic (forward stepper from stepper-demo.akk)")
    print("=" * 60)
    results = {
        "short_walk": test_short_walk_with_wav(),
        "long_run":   test_long_run_walks_arbitrary_count(),
    }
    print()
    print("=" * 60)
    failed = [k for k, v in results.items() if not v]
    if failed:
        print(f"FAIL — tests that did not pass: {failed}")
        print("If both fail, the bug is in counter or ARRAY_INDEX at the Cedar level.")
        print("If both pass, the bug is in Akkado lowering (closure / array literal / trigger).")
        raise SystemExit(1)
    print("PASS — forward stepper works at the Cedar level.")
    print("       → the demo's silent melody is therefore an Akkado-lowering bug,")
    print("         not a Cedar-opcode bug.")

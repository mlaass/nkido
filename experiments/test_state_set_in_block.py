"""
State Cell read-after-write Diagnostic (Cedar Engine)
=====================================================
Reproduces the *backward stepper* (`step_dir`) pattern from
`web/static/patches/stepper-demo.akk` at the Cedar VM level — no Akkado
compiler involved.

The Akkado source is:

    step_dir = (arr, trig, dir) -> {
      idx = state(0)
      idx.set(select(gateup(trig), idx.get() + dir, idx.get()))
      arr[idx.get()]
    }

This lowers to a STATE_OP rate=2 store whose input is
`select(gateup(trig), idx + dir, idx)`. By construction:

  * `gateup(trig)` is 1.0 on a single sample (the rising edge) and 0.0
    everywhere else.
  * `select(g, A, B) = A` where g > 0 else B.
  * STATE_OP rate=2 (`store`) writes ONLY `in[BLOCK_SIZE - 1]` to the slot
    (`cedar/include/cedar/opcodes/state_op.hpp:46`).

So the value written to the slot equals `select(...)[BLOCK_SIZE - 1]`,
which is `idx + dir` only if the rising edge happens to land exactly on
the last sample of a block — astronomically unlikely. Almost every block,
the slot is overwritten with the unchanged `idx` and the index never
advances.

This test reproduces that pattern with `dir = -1` (matching the demo's
backward bass) and asserts the slot walks `0, -1, -2, ...` per trigger.
The PREDICTION is that this test FAILS — that failure IS the diagnostic
for why the demo's bass voice never advances.

Per CLAUDE.md sequenced-opcode rule, the long-run check covers ≥ 300s.
"""

import numpy as np
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir


OUT = output_dir("op_state_set_in_block")
SR = 48000
DIR = -1.0
SLOT_ID = 0xC0DE
GATE_ID = cedar.hash("gateup_for_state_set_test")


def _make_state_inst(mode, out_buf, in0=0xFFFF, state_id=SLOT_ID):
    inst = cedar.Instruction.make_unary(cedar.Opcode.STATE_OP, out_buf, in0, state_id)
    inst.rate = mode
    return inst


def _make_gateup(out_buf, sig_buf, state_id=GATE_ID):
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EDGE_OP, out_buf, sig_buf, 0xFFFF, 0xFFFF, state_id
    )
    inst.rate = 1
    return inst


def _f32_to_u32(x: float) -> int:
    import struct
    return struct.unpack("<I", struct.pack("<f", x))[0]


def _push_const(buf: int, value: float) -> cedar.Instruction:
    inst = cedar.Instruction.make_nullary(cedar.Opcode.PUSH_CONST, buf)
    inst.state_id = _f32_to_u32(value)
    return inst


def _build_program():
    """
    Mirror the lowered step_dir body. Buffer map:

        0  external trigger input (set per block)
        1  PUSH_CONST 0  (initial value for state cell)
        2  STATE_OP rate=0 init out
        3  EDGE_OP rate=1 (gateup) out
        4  STATE_OP rate=1 load A  (idx.get() — first read for `idx + dir`)
        5  PUSH_CONST DIR
        6  ADD A + DIR  (idx + dir)
        7  STATE_OP rate=1 load B  (idx.get() — second read for the else-branch)
        8  SELECT(gate, idx+dir, idx) → input to set()
        9  STATE_OP rate=2 store from 8
       10  STATE_OP rate=1 load C  (idx.get() — final read returned to caller)

    OUTPUT writes buffer 10 to the left output channel.
    """
    program = []
    program.append(_push_const(1, 0.0))                          # init value
    program.append(_make_state_inst(0, 2, 1, SLOT_ID))           # state(0)
    program.append(_make_gateup(3, 0))                           # gateup(trig)
    program.append(_make_state_inst(1, 4, 0xFFFF, SLOT_ID))      # idx.get()
    program.append(_push_const(5, DIR))                          # dir constant
    program.append(cedar.Instruction.make_binary(                # idx + dir
        cedar.Opcode.ADD, 6, 4, 5))
    program.append(_make_state_inst(1, 7, 0xFFFF, SLOT_ID))      # idx.get() (else)
    program.append(cedar.Instruction.make_ternary(               # select
        cedar.Opcode.SELECT, 8, 3, 6, 7))
    program.append(_make_state_inst(2, 9, 8, SLOT_ID))           # idx.set(...)
    program.append(_make_state_inst(1, 10, 0xFFFF, SLOT_ID))     # idx.get() (final)
    program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10))
    return program


def _trigger_block(block_idx: int, period: int) -> np.ndarray:
    block = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
    base = block_idx * cedar.BLOCK_SIZE
    for i in range(cedar.BLOCK_SIZE):
        s = base + i
        if s > 0 and s % period == 0:
            block[i] = 1.0
    return block


def test_short_walk():
    """
    ~5s @ 5 Hz triggers. After N triggers, slot should hold N * DIR = -N.
    """
    print("\n[short] step_dir slot walk, ~5s @ 5 Hz triggers")
    print("-" * 60)

    host = CedarTestHost(SR)
    host.program = _build_program()
    host.vm.load_program(host.program)

    duration_s = 5
    n_blocks = duration_s * SR // cedar.BLOCK_SIZE
    period = SR // 5
    pulses = 0
    last_value = 0.0

    for block in range(n_blocks):
        trig = _trigger_block(block, period)
        pulses += int(np.sum(trig > 0.5))
        host.vm.set_buffer(0, trig)
        l, _ = host.vm.process()
        last_value = float(l[-1])

    expected = pulses * DIR
    ok = abs(last_value - expected) < 0.5
    sym = "✓" if ok else "✗"
    print(f"  {sym} after {duration_s}s ({pulses} pulses): slot={last_value:.1f}, expected={expected:.1f}")
    if not ok:
        print(f"  ✗ FAIL — slot did not advance.")
        print(f"     This is the predicted failure: STATE_OP rate=2 stores in[BLOCK_SIZE-1]")
        print(f"     only, so the per-sample 'increment on rising edge' is dropped unless")
        print(f"     the edge lands on the very last sample of a block (~1 in {cedar.BLOCK_SIZE}).")
    return ok


def test_long_run():
    """
    ≥ 300s @ 4 Hz triggers. Same assertion as the short test, just longer.
    """
    print("\n[long] step_dir slot walk, 300s @ 4 Hz triggers")
    print("-" * 60)

    host = CedarTestHost(SR)
    host.program = _build_program()
    host.vm.load_program(host.program)

    duration_s = 300
    n_blocks = duration_s * SR // cedar.BLOCK_SIZE
    period = SR // 4
    pulses = 0
    last_value = 0.0

    for block in range(n_blocks):
        trig = _trigger_block(block, period)
        pulses += int(np.sum(trig > 0.5))
        host.vm.set_buffer(0, trig)
        l, _ = host.vm.process()
        last_value = float(l[-1])

    expected = pulses * DIR
    ok = abs(last_value - expected) < 0.5
    sym = "✓" if ok else "✗"
    print(f"  {sym} after {duration_s}s ({pulses} pulses): slot={last_value:.1f}, expected={expected:.1f}")
    return ok


def test_diagnostic_proof():
    """
    Documents the STATE_OP rate=2 (store) contract: the slot becomes the
    LATEST sample of inputs[0] whose value differs from the slot's value at
    the start of the block. Samples that equal the start-of-block value are
    no-ops; this is what makes `idx.set(select(gateup(t), x+dir, x))` work
    when the rising-edge sample isn't the last sample of the block.

    Two probes:
      1. store([1, 0, 0, ..., 0]) with slot starting at 0
         sample 0: 1 differs from initial 0 → slot becomes 1
         samples 1..127: 0 == initial → skipped
         expected slot after the block: 1.0
      2. store([0, 0, ..., 0, 1]) with slot starting at 1 (carried over)
         samples 0..126: 0 differs from initial 1 → slot tracks down to 0
         sample 127: 1 differs from running slot, BUT initial was 1
                     → 1 == initial → skipped under the snapshot semantic
         expected slot after the block: 0.0
    """
    print("\n[proof] STATE_OP store records latest sample differing from start-of-block")
    print("-" * 60)

    host = CedarTestHost(SR)
    sid = 0xBEEF
    program = []
    program.append(_push_const(1, 0.0))                          # init value
    program.append(_make_state_inst(0, 2, 1, sid))               # state(0)
    # Buffer 3 will be set per block.
    program.append(_make_state_inst(2, 4, 3, sid))               # set(3)
    program.append(_make_state_inst(1, 5, 0xFFFF, sid))          # get
    program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 5))
    host.program = program
    host.vm.load_program(program)

    one_then_zero = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
    one_then_zero[0] = 1.0
    host.vm.set_buffer(3, one_then_zero)
    l, _ = host.vm.process()
    after_one_then_zero = float(l[-1])

    # For probe 2 we want slot to start at 1; the previous block left it at 1.
    nonzero_only_last = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
    nonzero_only_last[-1] = 1.0
    host.vm.set_buffer(3, nonzero_only_last)
    l, _ = host.vm.process()
    after_nonzero_last = float(l[-1])

    print(f"  store([1, 0, ..., 0]) with slot starting 0 → slot = {after_one_then_zero:.3f}")
    print(f"     expect 1.0 (sample 0 differs from initial 0; rest equal initial = skipped)")
    print(f"  store([0, 0, ..., 1]) with slot starting 1 → slot = {after_nonzero_last:.3f}")
    print(f"     expect 0.0 (samples 0..126 differ from initial 1; sample 127 = initial = skipped)")

    probe1_ok = abs(after_one_then_zero - 1.0) < 1e-5
    probe2_ok = abs(after_nonzero_last - 0.0) < 1e-5

    if probe1_ok and probe2_ok:
        print(f"  ✓ confirmed: STATE_OP rate=2 records latest sample != start-of-block value")
        print(f"  → this is what lets `idx.set(select(gateup(t), x+dir, x))` advance idx by")
        print(f"     dir on every rising edge, regardless of which sample carries the edge.")
        return True
    else:
        print(f"  ⚠ STATE_OP store semantics unclear — investigate state_op.hpp")
        if not probe1_ok: print(f"    probe1 failed: got {after_one_then_zero}, expected 1.0")
        if not probe2_ok: print(f"    probe2 failed: got {after_nonzero_last}, expected 0.0")
        return False


if __name__ == "__main__":
    print("Cedar State-Cell Read-After-Write Diagnostic")
    print("(Reproduces the step_dir bass voice from stepper-demo.akk)")
    print("=" * 60)

    results = {
        "diagnostic_proof": test_diagnostic_proof(),
        "short_walk": test_short_walk(),
        "long_run":   test_long_run(),
    }
    print()
    print("=" * 60)
    failed = [k for k, v in results.items() if not v and k != "diagnostic_proof"]
    if not results["diagnostic_proof"]:
        print("FAIL — could not confirm the read-after-write semantic.")
        raise SystemExit(2)
    if failed:
        print(f"FAIL (expected) — slot-walk tests did not advance: {failed}")
        print()
        print("DIAGNOSIS: STATE_OP rate=2 only writes the LAST sample of inputs[0]")
        print("to the slot. When the input is `select(gateup(trig), x+dir, x)`, that")
        print("last sample is almost always the unchanged x — the increment is")
        print("dropped in ~127/128 blocks. The userspace `step_dir` pattern as")
        print("written in stepper-demo.akk cannot work with these primitives.")
        print()
        print("Possible fixes (for the user to choose):")
        print("  1. Change STATE_OP rate=2 semantics to 'write the LAST sample where")
        print("     the input differs from the slot value' (or 'first non-zero gated').")
        print("  2. Add a new opcode `set_if(gate, value)` for gated writes.")
        print("  3. Rewrite step_dir to use `counter(trig) * dir + arr_offset` instead")
        print("     of state cells (mirrors how `step` works).")
        raise SystemExit(1)
    print("PASS — state cells walk correctly. (Unexpected; investigate further.)")

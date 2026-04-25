"""
STATE_OP Quality Test (Cedar Engine)
=====================================
Tests user state-cell I/O — Phase 3 of prd-userspace-state-and-edge-primitives.md.

Modes (selected via inst.rate, see cedar/include/cedar/opcodes/state_op.hpp):
  rate=0 init  — first execution writes inputs[0][0] to slot, sets initialized
  rate=1 load  — output = broadcast of slot value
  rate=2 store — write LAST sample of inputs[0] to slot, output = broadcast of new value

Long-run rule (CLAUDE.md): cells holding sequenced state should remain stable
across ≥ 300s simulated audio. We verify this by writing a counter sequence.
"""

import numpy as np
import os
import scipy.io.wavfile
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_state")
SR = 48000


def _make_state(mode, out_buf, in0=0xFFFF, state_id=None):
    if state_id is None:
        state_id = cedar.hash(f"state_test_{mode}")
    inst = cedar.Instruction.make_unary(
        cedar.Opcode.STATE_OP, out_buf, in0, state_id
    )
    inst.rate = mode
    return inst


def test_init_writes_once():
    print("\n[mode 0] init — fires once, value frozen")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_in, buf_out = 0, 1
    host.program.append(_make_state(0, buf_out, buf_in, state_id=0xDEAD))
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))
    host.vm.load_program(host.program)

    # Block 1: input = 7.5
    host.vm.set_buffer(buf_in, np.full(cedar.BLOCK_SIZE, 7.5, dtype=np.float32))
    l, _ = host.vm.process()
    ok1 = abs(l[-1] - 7.5) < 1e-5

    # Block 2: input = 99 (should be ignored, slot stays 7.5)
    host.vm.set_buffer(buf_in, np.full(cedar.BLOCK_SIZE, 99.0, dtype=np.float32))
    l, _ = host.vm.process()
    ok2 = abs(l[-1] - 7.5) < 1e-5

    sym1 = "✓" if ok1 else "✗"
    sym2 = "✓" if ok2 else "✗"
    print(f"  {sym1} block 1 init: out={l[-1]:.4f} → 7.5")  # block 2 stored above
    print(f"  {sym2} block 2 (input changed): slot still 7.5")
    return ok1 and ok2


def test_store_reads_last_sample():
    print("\n[mode 2] store — only the last sample wins")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_init, buf_store, buf_init_out, buf_store_out = 0, 1, 2, 3
    sid = 0xBEEF
    host.program.append(_make_state(0, buf_init_out, buf_init, state_id=sid))
    host.program.append(_make_state(2, buf_store_out, buf_store, state_id=sid))
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_store_out))
    host.vm.load_program(host.program)

    host.vm.set_buffer(buf_init, np.zeros(cedar.BLOCK_SIZE, dtype=np.float32))
    # store input: ramp 0..1, last sample = 127/128
    ramp = np.arange(cedar.BLOCK_SIZE, dtype=np.float32) / float(cedar.BLOCK_SIZE)
    host.vm.set_buffer(buf_store, ramp)
    l, _ = host.vm.process()

    expected = ramp[-1]
    ok = abs(l[-1] - expected) < 1e-5
    sym = "✓" if ok else "✗"
    print(f"  {sym} store output last sample: {l[-1]:.6f} (expected {expected:.6f})")
    return ok


def test_long_run_stability():
    """
    300+ seconds of simulated audio: store an incrementing sequence into a
    state cell and verify the slot tracks the latest written value across
    millions of samples. Per CLAUDE.md sequenced-opcode rule.
    """
    print("\n[long run] store stability over 300s simulated audio")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_init, buf_store, buf_init_out, buf_store_out, buf_load = 0, 1, 2, 3, 4
    sid = 0xCAFE
    host.program.append(_make_state(0, buf_init_out, buf_init, state_id=sid))
    host.program.append(_make_state(2, buf_store_out, buf_store, state_id=sid))
    host.program.append(_make_state(1, buf_load, 0xFFFF, state_id=sid))
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_load))
    host.vm.load_program(host.program)

    host.vm.set_buffer(buf_init, np.zeros(cedar.BLOCK_SIZE, dtype=np.float32))

    duration_s = 300
    n_blocks = duration_s * SR // cedar.BLOCK_SIZE
    # Each block writes its block index as the new slot value (last sample).
    last_load = 0.0
    for block in range(n_blocks):
        store_buf = np.full(cedar.BLOCK_SIZE, float(block), dtype=np.float32)
        host.vm.set_buffer(buf_store, store_buf)
        l, _ = host.vm.process()
        last_load = float(l[-1])

    # Final loaded value should equal the last block index (n_blocks - 1).
    expected = float(n_blocks - 1)
    ok = abs(last_load - expected) < 0.5
    sym = "✓" if ok else "✗"
    print(f"  {sym} after {duration_s}s ({n_blocks} blocks): loaded={last_load:.0f} expected={expected:.0f}")
    return ok


def test_audible_demo():
    """Make a WAV file showing state in action — a stair-step DC where each
    block stores a new value into the cell and we play back the cell."""
    print("\n[audible] stair-step DC from state cell loads")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_init, buf_store, buf_init_out, buf_store_out = 0, 1, 2, 3
    sid = 0xFADE
    host.program.append(_make_state(0, buf_init_out, buf_init, state_id=sid))
    host.program.append(_make_state(2, buf_store_out, buf_store, state_id=sid))
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_store_out))
    host.vm.load_program(host.program)

    host.vm.set_buffer(buf_init, np.zeros(cedar.BLOCK_SIZE, dtype=np.float32))

    n_seconds = 1
    n_blocks = n_seconds * SR // cedar.BLOCK_SIZE
    out = []
    for block in range(n_blocks):
        # Step value over each second: ascending values
        v = (block / n_blocks) * 0.8 - 0.4  # -0.4 .. 0.4
        host.vm.set_buffer(buf_store, np.full(cedar.BLOCK_SIZE, v, dtype=np.float32))
        l, _ = host.vm.process()
        out.append(l)
    wave = np.concatenate(out)
    wav_path = os.path.join(OUT, "state_stairs.wav")
    scipy.io.wavfile.write(wav_path, SR, wave.astype(np.float32))
    print(f"  Saved {wav_path} - listen for ascending DC stairs")
    return True


if __name__ == "__main__":
    print("Cedar STATE_OP Quality Test")
    print("=" * 60)
    results = {
        "init": test_init_writes_once(),
        "store": test_store_reads_last_sample(),
        "long_run": test_long_run_stability(),
        "audible": test_audible_demo(),
    }
    print()
    print("=" * 60)
    failed = [k for k, v in results.items() if not v]
    if failed:
        print(f"FAIL: {failed}")
        raise SystemExit(1)
    print("PASS — STATE_OP behaves correctly.")

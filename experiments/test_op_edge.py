"""
EDGE_OP Quality Test (Cedar Engine)
====================================
Tests all four EDGE_OP modes (rate=0..3): SAH, gateup, gatedown, counter.

Expected behavior (per cedar/include/cedar/opcodes/edge_op.hpp):
- rate=0 sah(in, trig)            : holds input value across rising edges of trig
- rate=1 gateup(sig)              : 1.0 single-sample pulse on rising edges
- rate=2 gatedown(sig)            : 1.0 single-sample pulse on falling edges
- rate=3 counter(trig, reset?, start?) : increments on rising edge of trig;
                                         reset to start (or 0) on rising edge of reset.
                                         Reset wins if both fire on the same sample.

If a test fails, investigate the implementation, do NOT change the test threshold.

Per CLAUDE.md sequenced-opcode rule, the counter test runs ≥ 300s of simulated
audio to surface long-tail bugs.
"""

import numpy as np
import os
import scipy.io.wavfile
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_edge")
SR = 48000


def _zero_buffer():
    return np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)


def _process_blocks(host, sig_buf_idx, trig_buf_idx, sig, trig,
                    extra_inputs=None, num_blocks=None):
    """
    Run the loaded program over `num_blocks` blocks, feeding `sig` to
    `sig_buf_idx` and `trig` to `trig_buf_idx`. Returns the concatenated
    left-channel output.

    `extra_inputs` is a dict {buffer_idx: full-length-numpy-array}.
    """
    if num_blocks is None:
        num_blocks = (len(sig) + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    padded = num_blocks * cedar.BLOCK_SIZE

    sig_p = np.zeros(padded, dtype=np.float32)
    sig_p[:len(sig)] = sig
    trig_p = np.zeros(padded, dtype=np.float32)
    trig_p[:len(trig)] = trig

    extra_p = {}
    if extra_inputs:
        for k, v in extra_inputs.items():
            arr = np.zeros(padded, dtype=np.float32)
            arr[:len(v)] = v
            extra_p[k] = arr

    out = []
    for i in range(num_blocks):
        s, e = i * cedar.BLOCK_SIZE, (i + 1) * cedar.BLOCK_SIZE
        host.vm.set_buffer(sig_buf_idx, sig_p[s:e])
        host.vm.set_buffer(trig_buf_idx, trig_p[s:e])
        for k, v in extra_p.items():
            host.vm.set_buffer(k, v[s:e])
        l, _ = host.vm.process()
        out.append(l)
    return np.concatenate(out)[:padded]


def _make_edge_inst(mode, out_buf, in0, in1=0xFFFF, in2=0xFFFF, state_id=None):
    """Build an EDGE_OP instruction with the given rate/mode."""
    if state_id is None:
        state_id = cedar.hash(f"edge_op_test_{mode}")
    # Use the factory helpers — they set unused inputs to 0xFFFF correctly
    # (default-constructing Instruction() leaves them at 0, which would alias
    # buffer 0 and silently break optional-input checks).
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EDGE_OP, out_buf, in0, in1, in2, state_id
    )
    inst.rate = mode
    return inst


def test_sah_mode():
    """rate=0: classic sample-and-hold. Same behavior as the legacy SAH test."""
    print("\n[mode 0] SAH — sample and hold")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_in, buf_trig, buf_out = 0, 1, 2
    inst = _make_edge_inst(0, buf_out, buf_in, buf_trig)
    host.program.append(inst)
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))
    host.vm.load_program(host.program)

    duration = 1.0
    n = int(duration * SR)
    ramp = np.linspace(0.0, 1.0, n, dtype=np.float32)

    # Trigger at 100ms, 250ms, 500ms, 750ms — should hold ramp[trig_sample] each time.
    trigger = np.zeros(n, dtype=np.float32)
    trig_samples = [int(SR * t) for t in (0.1, 0.25, 0.5, 0.75)]
    for ts in trig_samples:
        trigger[ts] = 1.0

    out = _process_blocks(host, buf_in, buf_trig, ramp, trigger)

    wav_path = os.path.join(OUT, "sah.wav")
    scipy.io.wavfile.write(wav_path, SR, out[:n])
    print(f"  Saved {wav_path} - listen for stair-step staircase rising over 1s")

    all_pass = True
    for ts in trig_samples:
        # 100 samples after trigger should equal the ramp value at trigger
        check = out[ts + 100] if ts + 100 < n else float("nan")
        expected = ramp[ts]
        ok = abs(check - expected) < 1e-4
        all_pass &= ok
        sym = "✓" if ok else "✗"
        print(f"  {sym} trig at {ts/SR*1000:.0f}ms: held={check:.4f}, expected={expected:.4f}")
    print("  ✓ PASS: all SAH holds correct" if all_pass else "  ✗ FAIL")
    return all_pass


def test_gateup_mode():
    """rate=1: 1.0 pulse on every rising edge."""
    print("\n[mode 1] gateup — rising edge detector")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_sig, buf_unused, buf_out = 0, 1, 2
    inst = _make_edge_inst(1, buf_out, buf_sig)
    host.program.append(inst)
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))
    host.vm.load_program(host.program)

    # 2-second signal with rising edges at 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75 s
    # (square wave at 4 Hz → 4 cycles in 1s = 4 rising edges; here 2s → 8 rising edges,
    # but the very first sample also counts, so we expect 8.)
    n = 2 * SR
    sig = np.zeros(n, dtype=np.float32)
    period = SR // 4  # 4 Hz square
    for i in range(n):
        sig[i] = 1.0 if (i % period) < (period // 2) else 0.0
    # Note: with prev_trigger init = 0 and sig[0]=1, sample 0 is a rising edge.
    expected_pulses = 8  # 8 rising edges in 2s @ 4 Hz including the first one

    out = _process_blocks(host, buf_sig, buf_unused, sig, np.zeros(n, dtype=np.float32))

    wav_path = os.path.join(OUT, "gateup.wav")
    scipy.io.wavfile.write(wav_path, SR, out[:n])
    print(f"  Saved {wav_path} - listen for 8 sharp clicks")

    pulses = int(np.sum(out[:n] > 0.5))
    ok = pulses == expected_pulses
    sym = "✓" if ok else "✗"
    print(f"  {sym} pulse count: {pulses} (expected {expected_pulses})")
    if ok:
        print("  ✓ PASS: gateup pulse count correct")
    else:
        print("  ✗ FAIL: investigate edge_op.hpp rate=1 case")
    return ok


def test_gatedown_mode():
    """rate=2: 1.0 pulse on every falling edge."""
    print("\n[mode 2] gatedown — falling edge detector")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_sig, buf_unused, buf_out = 0, 1, 2
    inst = _make_edge_inst(2, buf_out, buf_sig)
    host.program.append(inst)
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))
    host.vm.load_program(host.program)

    n = 2 * SR
    sig = np.zeros(n, dtype=np.float32)
    period = SR // 4
    for i in range(n):
        sig[i] = 1.0 if (i % period) < (period // 2) else 0.0
    # 8 falling edges in 2s @ 4 Hz (first half-cycle ends at sample period//2)
    expected_pulses = 8

    out = _process_blocks(host, buf_sig, buf_unused, sig, np.zeros(n, dtype=np.float32))

    wav_path = os.path.join(OUT, "gatedown.wav")
    scipy.io.wavfile.write(wav_path, SR, out[:n])
    print(f"  Saved {wav_path} - listen for 8 sharp clicks (offset from gateup)")

    pulses = int(np.sum(out[:n] > 0.5))
    ok = pulses == expected_pulses
    sym = "✓" if ok else "✗"
    print(f"  {sym} pulse count: {pulses} (expected {expected_pulses})")
    if ok:
        print("  ✓ PASS: gatedown pulse count correct")
    else:
        print("  ✗ FAIL")
    return ok


def test_counter_long_run():
    """
    rate=3: counter increments on rising edges. Per CLAUDE.md, sequenced/stateful
    opcodes need ≥ 300s simulated audio. We trigger at 1 Hz over 5 minutes (no
    WAV write — file size would balloon and counter is silent anyway), and
    verify the final value matches the trigger count exactly.
    """
    print("\n[mode 3] counter — long-run accumulator (300s)")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_trig, buf_reset, buf_start, buf_out = 0, 1, 2, 3
    inst = _make_edge_inst(3, buf_out, buf_trig, buf_reset, buf_start)
    host.program.append(inst)
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))
    host.vm.load_program(host.program)

    duration_s = 300
    n_blocks = duration_s * SR // cedar.BLOCK_SIZE
    # Pulses fire at samples SR, 2*SR, ..., (duration_s-1)*SR.
    # The loop only processes samples 0..duration_s*SR-1, so the final pulse
    # at sample duration_s*SR is never reached — that's duration_s-1 pulses.
    expected_count = duration_s - 1

    last_count = 0.0
    for block in range(n_blocks):
        block_start_sample = block * cedar.BLOCK_SIZE
        trig = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
        # Place a single-sample pulse at the boundary of each whole second.
        for s_in_block in range(cedar.BLOCK_SIZE):
            global_sample = block_start_sample + s_in_block
            if global_sample > 0 and global_sample % SR == 0:
                trig[s_in_block] = 1.0
        host.vm.set_buffer(buf_trig, trig)
        host.vm.set_buffer(buf_reset, _zero_buffer())
        host.vm.set_buffer(buf_start, _zero_buffer())
        l, _ = host.vm.process()
        last_count = float(l[-1])

    ok = abs(last_count - expected_count) < 0.5
    sym = "✓" if ok else "✗"
    print(f"  {sym} after {duration_s}s @ 1 Hz: counter={last_count:.1f}, expected={expected_count}")
    if ok:
        print("  ✓ PASS: counter reaches correct value over long run")
    else:
        print("  ✗ FAIL: investigate counter accumulator stability")
    return ok


def test_counter_short():
    """
    Short shapeable counter test: increments + reset + start, with WAV output for
    audible debugging.
    """
    print("\n[mode 3] counter — short test with reset and start")
    print("-" * 60)

    host = CedarTestHost(SR)
    buf_trig, buf_reset, buf_start, buf_out = 0, 1, 2, 3
    inst = _make_edge_inst(3, buf_out, buf_trig, buf_reset, buf_start)
    host.program.append(inst)
    host.program.append(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))
    host.vm.load_program(host.program)

    n = SR  # 1 second
    # Ten increments evenly spaced over the second
    trig = np.zeros(n, dtype=np.float32)
    inc_samples = np.linspace(SR // 20, n - SR // 20, 10).astype(int)
    trig[inc_samples] = 1.0
    # Reset at 0.5s, start = 100
    reset = np.zeros(n, dtype=np.float32)
    reset[SR // 2] = 1.0
    start = np.full(n, 100.0, dtype=np.float32)

    out = _process_blocks(host, buf_trig, buf_reset, trig, reset,
                          extra_inputs={buf_start: start})

    wav_path = os.path.join(OUT, "counter.wav")
    # Normalize loudness for the WAV (counter goes high)
    peak = float(np.max(np.abs(out[:n]))) or 1.0
    scipy.io.wavfile.write(wav_path, SR, (out[:n] / peak).astype(np.float32))
    print(f"  Saved {wav_path} - listen for stair-step DC")

    # First half: counter should reach 5 by sample SR//2-1 (5 of the 10 triggers fired).
    # At sample SR//2 the reset fires (and may overlap with a trigger but reset wins).
    # Then the remaining triggers count from 100 upward.
    final = float(out[n - 1])
    # 10 triggers total, 5 fire before reset (reach 5), reset to 100, remaining 5 → 105.
    expected_final = 105.0
    ok = abs(final - expected_final) < 0.5
    sym = "✓" if ok else "✗"
    print(f"  {sym} final counter: {final:.1f}, expected {expected_final:.1f}")
    if ok:
        print("  ✓ PASS: counter reset + start behave correctly")
    else:
        print("  ✗ FAIL: investigate edge_op.hpp rate=3 case")
    return ok


if __name__ == "__main__":
    print("Cedar EDGE_OP Quality Test (all 4 modes)")
    print("=" * 60)
    results = {
        "sah":      test_sah_mode(),
        "gateup":   test_gateup_mode(),
        "gatedown": test_gatedown_mode(),
        "counter_short": test_counter_short(),
        "counter_long":  test_counter_long_run(),
    }
    print()
    print("=" * 60)
    failed = [k for k, v in results.items() if not v]
    if failed:
        print(f"FAIL — modes that did not pass: {failed}")
        raise SystemExit(1)
    print("PASS — all EDGE_OP modes verified.")

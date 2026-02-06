"""
Test: MS_ENCODE (Mid/Side Encoding)
====================================
Tests MS_ENCODE opcode for correct mid/side separation.

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp):
- M = (L + R) / 2
- S = (L - R) / 2

If this test fails, check the implementation in cedar/include/cedar/opcodes/stereo.hpp
"""

import os
import numpy as np

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_ms_encode")


# =============================================================================
# Helper Functions
# =============================================================================

def gen_mono_sine(freq, duration, sr):
    """Generate a mono sine wave."""
    t = np.arange(int(duration * sr)) / sr
    return np.sin(2 * np.pi * freq * t).astype(np.float32)


def gen_stereo_test_signal(duration, sr):
    """Generate a test stereo signal with different content in L and R."""
    t = np.arange(int(duration * sr)) / sr
    # Left: 440 Hz sine
    left = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.7
    # Right: 550 Hz sine (different frequency)
    right = np.sin(2 * np.pi * 550 * t).astype(np.float32) * 0.7
    return left, right


# =============================================================================
# Tests
# =============================================================================

def test_ms_encode():
    """
    Test MS_ENCODE opcode for correct mid/side separation.

    Acceptance criteria:
    - Mid channel error < 1e-6
    - Side channel error < 1e-6
    - Center mono signal produces zero side channel
    - Hard-panned signal produces equal mid and side levels
    """
    print("Test: MS_ENCODE Mid/Side Separation")

    sr = 48000
    duration = 0.5

    # --- Test 1: Verify M/S math on stereo signal ---
    print("\n  Verifying M/S separation on stereo signal:")
    left_in, right_in = gen_stereo_test_signal(duration, sr)

    host = CedarTestHost(sr)

    # MS_ENCODE: buf0,1 -> buf3,4 (mid/side)
    inst_encode = cedar.Instruction.make_binary(
        cedar.Opcode.MS_ENCODE, 3, 0, 1, cedar.hash("ms_enc") & 0xFFFF
    )
    host.load_instruction(inst_encode)

    # Output M and S directly
    host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 3, 4))

    mid_out, side_out = host.process_stereo(left_in, right_in)

    # Expected values
    expected_mid = (left_in + right_in) / 2
    expected_side = (left_in - right_in) / 2

    mid_error = np.sqrt(np.mean((mid_out - expected_mid)**2))
    side_error = np.sqrt(np.mean((side_out - expected_side)**2))

    print(f"  Mid channel error: {mid_error:.2e}")
    print(f"  Side channel error: {side_error:.2e}")

    if mid_error < 1e-6 and side_error < 1e-6:
        print(f"  ✓ PASS: M/S encoding mathematically correct")
    else:
        print(f"  ✗ FAIL: M/S encoding has errors")

    # --- Test 2: Center mono signal should produce zero side ---
    print("\n  Verifying center mono produces zero side:")
    mono = gen_mono_sine(440, duration, sr)

    host2 = CedarTestHost(sr)
    inst_encode2 = cedar.Instruction.make_binary(
        cedar.Opcode.MS_ENCODE, 3, 0, 1, cedar.hash("ms_enc_mono") & 0xFFFF
    )
    host2.load_instruction(inst_encode2)
    host2.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 3, 4))

    mid_mono, side_mono = host2.process_stereo(mono, mono)

    side_rms = np.sqrt(np.mean(side_mono**2))
    mid_rms = np.sqrt(np.mean(mid_mono**2))

    print(f"  Mid RMS: {mid_rms:.4f}")
    print(f"  Side RMS: {side_rms:.2e}")

    if side_rms < 1e-6:
        print(f"  ✓ PASS: Center mono produces zero side channel")
    else:
        print(f"  ✗ FAIL: Side channel not zero for center mono (RMS={side_rms:.2e})")

    # --- Test 3: Hard-panned signal should have equal mid and side ---
    print("\n  Verifying hard-left signal produces equal mid/side levels:")
    left_only = gen_mono_sine(440, duration, sr)
    silence = np.zeros(int(duration * sr), dtype=np.float32)

    host3 = CedarTestHost(sr)
    inst_encode3 = cedar.Instruction.make_binary(
        cedar.Opcode.MS_ENCODE, 3, 0, 1, cedar.hash("ms_enc_hard") & 0xFFFF
    )
    host3.load_instruction(inst_encode3)
    host3.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 3, 4))

    mid_hard, side_hard = host3.process_stereo(left_only, silence)

    mid_hard_rms = np.sqrt(np.mean(mid_hard**2))
    side_hard_rms = np.sqrt(np.mean(side_hard**2))
    ratio = mid_hard_rms / (side_hard_rms + 1e-10)

    print(f"  Mid RMS: {mid_hard_rms:.4f}")
    print(f"  Side RMS: {side_hard_rms:.4f}")
    print(f"  Mid/Side ratio: {ratio:.3f}")

    # For hard-left (L=signal, R=0): M = L/2, S = L/2, so ratio should be 1.0
    if 0.95 < ratio < 1.05:
        print(f"  ✓ PASS: Hard-panned signal has equal mid/side levels (ratio={ratio:.3f} ~= 1.0)")
    else:
        print(f"  ✗ FAIL: Mid/side ratio incorrect (ratio={ratio:.3f}, expected ~1.0)")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("=" * 60)
    print("MS_ENCODE OPCODE TESTS")
    print("=" * 60)

    print()
    test_ms_encode()

    print("\n" + "=" * 60)
    print("MS_ENCODE TESTS COMPLETE")
    print("=" * 60)

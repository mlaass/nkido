"""
Test: MS_DECODE (Mid/Side Decoding)
====================================
Tests MS_DECODE opcode for correct mid/side to stereo reconstruction.

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp):
- L = M + S
- R = M - S
- Roundtrip: MS_ENCODE -> MS_DECODE should be identity

If this test fails, check the implementation in cedar/include/cedar/opcodes/stereo.hpp
"""

import os
import numpy as np

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_ms_decode")


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

def test_ms_decode():
    """
    Test MS_DECODE opcode via encode->decode roundtrip.

    Expected behavior:
    - MS_ENCODE: M = (L+R)/2, S = (L-R)/2
    - MS_DECODE: L = M+S, R = M-S
    - Roundtrip should be perfect: encode -> decode = identity

    Acceptance criteria:
    - Roundtrip reconstruction error < -80dB for all test cases
    """
    print("Test: MS_DECODE Roundtrip Reconstruction")

    sr = 48000
    duration = 0.5

    test_cases = [
        ("Center Mono", lambda sr, d: (gen_mono_sine(440, d, sr), gen_mono_sine(440, d, sr))),
        ("Hard Left", lambda sr, d: (gen_mono_sine(440, d, sr), np.zeros(int(d * sr), dtype=np.float32))),
        ("Hard Right", lambda sr, d: (np.zeros(int(d * sr), dtype=np.float32), gen_mono_sine(440, d, sr))),
        ("Stereo Mix", lambda sr, d: gen_stereo_test_signal(d, sr)),
    ]

    all_passed = True

    for name, generator in test_cases:
        left_in, right_in = generator(sr, duration)

        host = CedarTestHost(sr)

        # MS_ENCODE: buf0,1 -> buf3,4 (mid/side)
        inst_encode = cedar.Instruction.make_binary(
            cedar.Opcode.MS_ENCODE, 3, 0, 1, cedar.hash("ms_enc") & 0xFFFF
        )
        host.load_instruction(inst_encode)

        # MS_DECODE: buf3,4 (mid/side) -> buf5,6 (L/R)
        inst_decode = cedar.Instruction.make_binary(
            cedar.Opcode.MS_DECODE, 5, 3, 4, cedar.hash("ms_dec") & 0xFFFF
        )
        host.load_instruction(inst_decode)

        # Output
        host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 5, 6))

        out_l, out_r = host.process_stereo(left_in, right_in)

        # Calculate error
        error_l = out_l - left_in
        error_r = out_r - right_in

        # RMS error in dB
        signal_power = (np.mean(left_in**2) + np.mean(right_in**2)) / 2
        error_power = (np.mean(error_l**2) + np.mean(error_r**2)) / 2
        error_db = 10 * np.log10(error_power / (signal_power + 1e-20) + 1e-20)

        if error_db < -80:
            print(f"  ✓ {name}: roundtrip error = {error_db:.1f}dB")
        else:
            print(f"  ✗ {name}: roundtrip error = {error_db:.1f}dB (should be < -80dB)")
            all_passed = False

    if all_passed:
        print(f"\n  ✓ ALL MS_DECODE ROUNDTRIP TESTS PASSED")
    else:
        print(f"\n  ✗ SOME MS_DECODE ROUNDTRIP TESTS FAILED")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("=" * 60)
    print("MS_DECODE OPCODE TESTS")
    print("=" * 60)

    print()
    test_ms_decode()

    print("\n" + "=" * 60)
    print("MS_DECODE TESTS COMPLETE")
    print("=" * 60)

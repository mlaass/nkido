"""
Test: MONO_DOWNMIX (Stereo-to-Mono Sum)
========================================
Tests MONO_DOWNMIX opcode — the Cedar counterpart of Akkado's mono() builtin
added by prd-stereo-support.md.

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp):
- out[i] = (L[i] + R[i]) * 0.5
- Correlated content (same signal in L and R) → output = original, 0 dB
- Fully uncorrelated content (indep. noise in L and R) → -3 dB RMS
- Anti-phase content (L = -R) → output = 0 (perfect cancellation)
- DC offsets sum: mono(L=0.5, R=0.3) → 0.4

If this test fails, check op_mono_downmix in cedar/include/cedar/opcodes/stereo.hpp
"""

import os
import numpy as np

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav

OUT = output_dir("op_mono_downmix")


def build_downmix_program(host):
    """Load a program: buf 0 = L input, buf 1 = R input, buf 2 = downmix, output to L only."""
    # MONO_DOWNMIX: out=buf2, in0=buf0 (L), in1=buf1 (R)
    host.load_instruction(cedar.Instruction.make_binary(
        cedar.Opcode.MONO_DOWNMIX, 2, 0, 1
    ))
    # Route the mono result to the output's left channel only so we can
    # inspect it directly from host.process_stereo's left output.
    host.load_instruction(cedar.Instruction.make_binary(
        cedar.Opcode.OUTPUT, 0, 2, 2
    ))


def test_correlated_input():
    """Identical L and R signals → output equals input at unity gain."""
    print("Test: correlated input (L == R)")
    sr = 48000
    host = CedarTestHost(sr)
    build_downmix_program(host)

    t = np.arange(int(0.25 * sr)) / sr
    sine = np.sin(2 * np.pi * 440 * t).astype(np.float32)

    out_l, _ = host.process_stereo(sine, sine)

    # output = (L+R)*0.5 = sine*1.0 = sine
    err = float(np.max(np.abs(out_l[:len(sine)] - sine)))
    wav_path = os.path.join(OUT, "correlated.wav")
    save_wav(wav_path, out_l[:len(sine)], sr)
    print(f"  Saved {wav_path} - Listen for a clean 440Hz sine")

    if err < 1e-5:
        print(f"  ✓ PASS: correlated downmix preserves signal (max err {err:.2e})")
    else:
        print(f"  ✗ FAIL: correlated downmix changed signal (max err {err:.2e})")


def test_anti_phase_input():
    """L = -R → output cancels to zero."""
    print("Test: anti-phase input (L == -R)")
    sr = 48000
    host = CedarTestHost(sr)
    build_downmix_program(host)

    t = np.arange(int(0.25 * sr)) / sr
    sine = np.sin(2 * np.pi * 440 * t).astype(np.float32)

    out_l, _ = host.process_stereo(sine, -sine)

    peak = float(np.max(np.abs(out_l[:len(sine)])))
    if peak < 1e-5:
        print(f"  ✓ PASS: anti-phase content cancels to silence (peak {peak:.2e})")
    else:
        print(f"  ✗ FAIL: anti-phase content did not cancel (peak {peak:.2e})")


def test_dc_sum():
    """DC L=0.5, R=0.3 → output = 0.4 constant."""
    print("Test: DC input (L=0.5, R=0.3)")
    sr = 48000
    host = CedarTestHost(sr)
    build_downmix_program(host)

    left = np.full(cedar.BLOCK_SIZE * 4, 0.5, dtype=np.float32)
    right = np.full(cedar.BLOCK_SIZE * 4, 0.3, dtype=np.float32)

    out_l, _ = host.process_stereo(left, right)

    mean_val = float(np.mean(out_l[:len(left)]))
    if abs(mean_val - 0.4) < 1e-5:
        print(f"  ✓ PASS: DC sum equals 0.4 (got {mean_val:.4f})")
    else:
        print(f"  ✗ FAIL: DC sum wrong (expected 0.4, got {mean_val:.4f})")


def test_uncorrelated_noise():
    """Independent noise → approx -3 dB RMS drop (sum-to-mono convention)."""
    print("Test: uncorrelated noise")
    sr = 48000
    host = CedarTestHost(sr)
    build_downmix_program(host)

    rng = np.random.default_rng(1234)
    n = 48000
    left = rng.standard_normal(n).astype(np.float32) * 0.25
    right = rng.standard_normal(n).astype(np.float32) * 0.25

    out_l, _ = host.process_stereo(left, right)

    rms_in = float(np.sqrt(np.mean(left**2)))  # same RMS as right
    rms_out = float(np.sqrt(np.mean(out_l[:n] ** 2)))

    drop_db = 20 * np.log10(rms_out / (rms_in + 1e-12) + 1e-12)
    # Expected: 10*log10(0.5) = -3.01 dB for independent equal-RMS sources
    if abs(drop_db - (-3.01)) < 0.5:
        print(f"  ✓ PASS: uncorrelated noise drops ~{drop_db:.2f} dB (expected -3 dB)")
    else:
        print(f"  ✗ FAIL: uncorrelated drop {drop_db:.2f} dB not near -3 dB")


if __name__ == "__main__":
    print("=" * 60)
    print("MONO_DOWNMIX OPCODE TESTS")
    print("=" * 60)
    print()
    test_correlated_input()
    print()
    test_anti_phase_input()
    print()
    test_dc_sum()
    print()
    test_uncorrelated_noise()
    print()
    print("=" * 60)
    print("MONO_DOWNMIX TESTS COMPLETE")
    print("=" * 60)

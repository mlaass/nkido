"""
Test: WIDTH (Stereo Width Control)
===================================
Tests WIDTH opcode for stereo width manipulation via mid/side processing.

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp):
- M = (L + R) / 2, S = (L - R) / 2
- L' = M + S * width, R' = M - S * width
- width=0: mono (L' = R' = M)
- width=1: original stereo
- width=2: exaggerated width (S doubled)

If this test fails, check the implementation in cedar/include/cedar/opcodes/stereo.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_width")


# =============================================================================
# Helper Functions
# =============================================================================

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

def test_width_stereo():
    """
    Test WIDTH opcode for stereo width control.

    Acceptance criteria:
    - At width=0: L/R correlation > 0.99 (mono)
    - At width=1: Output matches input (side_ratio ~= 1.0)
    - At width=2: side_ratio ~= 2.0
    """
    print("Test: WIDTH Stereo Width Control")

    sr = 48000
    duration = 1.0

    # Generate test stereo signal with known L/R content
    left_in, right_in = gen_stereo_test_signal(duration, sr)

    # Calculate input mid/side for reference
    mid_in = (left_in + right_in) / 2
    side_in = (left_in - right_in) / 2
    side_rms_in = np.sqrt(np.mean(side_in**2))

    width_values = [0.0, 0.5, 1.0, 1.5, 2.0]
    results = {}

    for width_val in width_values:
        host = CedarTestHost(sr)

        # Buffers: 0=left, 1=right, param buffer for width
        buf_left = 0
        buf_right = 1
        buf_width = host.set_param("width", width_val)

        # WIDTH: out_left=buf5, out_right=buf6
        # WIDTH(out, in_left, in_right, width)
        inst = cedar.Instruction.make_ternary(
            cedar.Opcode.WIDTH, 5, buf_left, buf_right, buf_width,
            cedar.hash("width_test") & 0xFFFF
        )
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 5, 6))

        out_l, out_r = host.process_stereo(left_in, right_in)

        # Calculate output mid/side
        mid_out = (out_l + out_r) / 2
        side_out = (out_l - out_r) / 2

        # Correlation between L and R (high = mono)
        correlation = np.corrcoef(out_l, out_r)[0, 1]

        # Side signal ratio
        side_rms_out = np.sqrt(np.mean(side_out**2))
        side_ratio = side_rms_out / (side_rms_in + 1e-10)

        results[width_val] = {
            'correlation': correlation,
            'side_ratio': side_ratio,
            'output_left': out_l,
            'output_right': out_r
        }

        print(f"  Width={width_val}: correlation={correlation:.4f}, side_ratio={side_ratio:.2f}")

    # Pass/fail checks
    # Width=0 should be mono
    if results[0.0]['correlation'] > 0.99:
        print(f"  ✓ PASS: Width=0 produces mono (correlation={results[0.0]['correlation']:.4f} > 0.99)")
    else:
        print(f"  ✗ FAIL: Width=0 not mono enough (correlation={results[0.0]['correlation']:.4f})")

    # Width=1 should preserve input
    if 0.95 < results[1.0]['side_ratio'] < 1.05:
        print(f"  ✓ PASS: Width=1 preserves stereo (side_ratio={results[1.0]['side_ratio']:.3f} ~= 1.0)")
    else:
        print(f"  ✗ FAIL: Width=1 doesn't preserve stereo (side_ratio={results[1.0]['side_ratio']:.3f})")

    # Width=2 should double side
    if 1.9 < results[2.0]['side_ratio'] < 2.1:
        print(f"  ✓ PASS: Width=2 doubles stereo (side_ratio={results[2.0]['side_ratio']:.3f} ~= 2.0)")
    else:
        print(f"  ✗ FAIL: Width=2 doesn't double stereo (side_ratio={results[2.0]['side_ratio']:.3f})")

    # Save WAV files
    for width_val in width_values:
        out_l = results[width_val]['output_left']
        out_r = results[width_val]['output_right']
        stereo = np.column_stack([out_l, out_r])
        wav_path = os.path.join(OUT, f"width_{width_val:.1f}.wav")
        save_wav(wav_path, stereo, sr)

    # Plot
    fig, axes = plt.subplots(2, 3, figsize=(15, 8))
    fig.suptitle("WIDTH Stereo Width Analysis")

    for idx, width_val in enumerate(width_values):
        ax = axes[idx // 3, idx % 3]
        out_l = results[width_val]['output_left']
        out_r = results[width_val]['output_right']

        # Lissajous (X-Y) plot shows stereo image
        ax.plot(out_l[:4096], out_r[:4096], 'b.', alpha=0.1, markersize=1)
        ax.set_xlabel('Left')
        ax.set_ylabel('Right')
        ax.set_title(f'Width={width_val} (corr={results[width_val]["correlation"]:.2f})')
        ax.set_aspect('equal')
        ax.set_xlim(-1, 1)
        ax.set_ylim(-1, 1)
        ax.axhline(0, color='k', linewidth=0.5)
        ax.axvline(0, color='k', linewidth=0.5)
        # Diagonal lines for reference
        ax.plot([-1, 1], [-1, 1], 'g--', alpha=0.3, label='Mono')
        ax.plot([-1, 1], [1, -1], 'r--', alpha=0.3, label='Side')

    # Summary in 6th subplot
    axes[1, 2].axis('off')
    axes[1, 2].text(0.1, 0.8, "WIDTH Effect:", fontsize=12, fontweight='bold')
    axes[1, 2].text(0.1, 0.6, "- width=0: Mono collapse (diagonal line)", fontsize=10)
    axes[1, 2].text(0.1, 0.45, "- width=1: Original stereo (ellipse)", fontsize=10)
    axes[1, 2].text(0.1, 0.3, "- width=2: Exaggerated (wider ellipse)", fontsize=10)
    axes[1, 2].text(0.1, 0.1, f"Input: 440Hz L, 550Hz R sines", fontsize=9, style='italic')

    plt.tight_layout()
    fig_path = os.path.join(OUT, "width_analysis.png")
    save_figure(fig, fig_path)
    print(f"  Saved {fig_path}")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("=" * 60)
    print("WIDTH OPCODE TESTS")
    print("=" * 60)

    print()
    test_width_stereo()

    print("\n" + "=" * 60)
    print("WIDTH TESTS COMPLETE")
    print("=" * 60)

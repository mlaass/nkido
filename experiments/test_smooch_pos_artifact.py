"""
Smooch tablePos modulation artifacts — diagnostic harness
=========================================================

Reproduces the artifact the user reported in the web smoketest when modulating
the `pos` slider on a `smooch("morph", freq, 0, pos)` voice. Three scenarios
isolate what's actually happening:

  A. Smooth audio-rate ramp:  pos[i] is written directly into the input
     buffer via vm.set_buffer(2, ...). This bypasses the EnvMap slew so
     the opcode sees a perfectly linear ramp.

  B. UI-cadence emulation:    pos is updated every ~16.7 ms (60 Hz, the
     web UI's typical push rate) via vm.set_param("pos", v). The EnvMap
     applies its 5 ms slew. This matches what the user actually does.

  C. Held positions:          pos is held at fixed values for 1 s each.
     This is the quiet baseline — anything heard here is the wavetable
     itself, not a modulation artifact.

For each scenario we save the WAV to listen to and compute:

  - Spectral distance: cosine distance between adjacent 1024-sample STFT
    frames. Baseline (scenario C while held) should be near zero. Spikes
    in A/B localize when the artifact occurs.

  - Sideband energy at multiples of the UI step rate (60 Hz) on scenario
    B's spectrum. Energy here = the UI cadence is leaking into audio.

If both A and B show spikes during sweeps but B has no UI-rate sidebands,
the cause is intrinsic to the wavetable / opcode (e.g., per-frame phase
mismatch). If only B shows them, the cause is param plumbing. If both are
silent except for clear UI-rate sidebands in B, the cause is param-cadence
aliasing.

Reference target: < -60 dB sideband energy at the UI step rate, and
spectral-distance peaks during sweeps within 10x of the held-position
baseline.
"""

import os
import math
from pathlib import Path

import numpy as np
import scipy.io.wavfile as wf

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav, linear_to_db


OUT = output_dir("op_smooch_pos_artifact")
WAV_PATH = str(Path(__file__).resolve().parents[1]
               / "web" / "static" / "wavetables" / "sine_to_saw.wav")
N = 2048


# ============================================================================
# Helpers
# ============================================================================


def build_program(host: CedarTestHost, bank_id: int,
                  freq_buf: int, phase_buf: int, pos_buf: int,
                  tag: str) -> int:
    """Wire a single smooch voice → output. Returns the smooch output buffer."""
    smooch_out = 100
    state_id = cedar.hash(f"smooch.{tag}")
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.OSC_WAVETABLE,
        smooch_out, freq_buf, phase_buf, pos_buf, state_id)
    inst.rate = bank_id
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(
        cedar.Opcode.OUTPUT, 0, smooch_out))
    return smooch_out


def render_blocks(host: CedarTestHost, n_samples: int,
                  set_pos_buffer=None, set_param_at=None) -> np.ndarray:
    """
    Run the loaded program over n_samples samples.

    set_pos_buffer: optional callable (block_index) -> ndarray[BLOCK_SIZE] of
        float32 to push into buffer 2 (the pos input). If None, no override.
    set_param_at: optional callable (block_index) -> None or float — if it
        returns a number, push it via vm.set_param("pos", v) for that block.
    """
    BLOCK = cedar.BLOCK_SIZE
    n_blocks = (n_samples + BLOCK - 1) // BLOCK
    output = np.zeros(n_blocks * BLOCK, dtype=np.float32)
    for b in range(n_blocks):
        if set_pos_buffer is not None:
            host.vm.set_buffer(2, set_pos_buffer(b))
        if set_param_at is not None:
            v = set_param_at(b)
            if v is not None:
                host.vm.set_param("pos", float(v))
        l, _ = host.vm.process()
        s = b * BLOCK
        output[s:s + BLOCK] = np.asarray(l)
    return output[:n_samples]


def stft_cosine_distance(signal: np.ndarray, win: int = 1024,
                          hop: int = 256) -> np.ndarray:
    """
    Cosine distance between adjacent magnitude-spectrogram frames.

    A perfectly steady tone has distance ~0 between adjacent frames. A
    morph-induced spectral change shows up as a peak in this curve.
    """
    nframes = (len(signal) - win) // hop
    window = np.hanning(win).astype(np.float32)
    mags = np.zeros((nframes, win // 2 + 1), dtype=np.float32)
    for i in range(nframes):
        s = i * hop
        mags[i] = np.abs(np.fft.rfft(signal[s:s + win] * window))
    # Normalize each frame so we measure shape, not amplitude.
    norms = np.linalg.norm(mags, axis=1) + 1e-12
    mags /= norms[:, None]
    distances = np.zeros(nframes - 1, dtype=np.float32)
    for i in range(nframes - 1):
        distances[i] = 1.0 - float(np.dot(mags[i], mags[i + 1]))
    return distances


def sideband_energy_at_rate(signal: np.ndarray, sr: int, fund_hz: float,
                             rate_hz: float, n_sidebands: int = 6) -> float:
    """
    Measure summed energy in bins at fund ± k*rate (k = 1..n_sidebands)
    relative to the fundamental's energy. Returns dB.

    A clean tone with no UI-cadence modulation has bins at fund only;
    AM modulation at rate_hz creates sidebands at fund ± rate_hz, fund ± 2·rate_hz...
    """
    fft_size = 1 << 17  # ~2.7 s @ 48k — bin width ≈ 0.366 Hz
    if len(signal) < fft_size:
        fft_size = 1 << int(np.floor(np.log2(len(signal))))
    n = len(signal) - fft_size
    start = max(0, n // 2)  # take a chunk from the middle
    chunk = signal[start:start + fft_size] * np.hanning(fft_size)
    spec = np.abs(np.fft.rfft(chunk))
    bin_hz = sr / fft_size

    fund_bin = int(round(fund_hz / bin_hz))
    fund_e = float(np.sum(spec[max(0, fund_bin - 2):fund_bin + 3] ** 2))
    if fund_e < 1e-20:
        return -200.0

    sb_e = 0.0
    for k in range(1, n_sidebands + 1):
        for sign in (-1, +1):
            target_bin = int(round((fund_hz + sign * k * rate_hz) / bin_hz))
            if 0 <= target_bin < len(spec):
                sb_e += float(np.sum(
                    spec[max(0, target_bin - 1):target_bin + 2] ** 2))

    return 10.0 * math.log10(sb_e / fund_e + 1e-20)


def report(name: str, output: np.ndarray, sr: int, fund_hz: float,
            ui_rate_hz: float | None = None) -> dict:
    """Print and return a metrics dict."""
    rms_v = float(np.sqrt(np.mean(output ** 2)))
    distances = stft_cosine_distance(output)
    dist_max = float(np.max(distances)) if len(distances) else 0.0
    dist_mean = float(np.mean(distances)) if len(distances) else 0.0
    sb_db = (sideband_energy_at_rate(output, sr, fund_hz, ui_rate_hz)
             if ui_rate_hz else None)

    print(f"  [{name}] RMS={rms_v:.4f}  "
          f"spectral-dist max={dist_max:.4f} mean={dist_mean:.4f}", end="")
    if sb_db is not None:
        print(f"  sidebands@{ui_rate_hz:g}Hz={sb_db:+.1f} dB", end="")
    print()
    return {
        "rms": rms_v, "dist_max": dist_max, "dist_mean": dist_mean,
        "sideband_db": sb_db,
    }


# ============================================================================
# Scenarios
# ============================================================================


def scenario_a_smooth_ramp(sr: int, freq_hz: float, duration_s: float):
    """A: smooth audio-rate ramp 0 → 31 over `duration_s` seconds."""
    print("Scenario A: smooth audio-rate pos ramp 0 → 31")
    host = CedarTestHost(sr)
    bank_id = host.vm.wavetable_load_wav("morph", WAV_PATH)
    freq_buf  = host.set_param("freq",  freq_hz)
    phase_buf = host.set_param("phase",   0.0)
    # We will override buffer 2 directly each block — pos_buf points to a
    # raw input buffer (slot 2 is read by the smooch instruction).
    build_program(host, bank_id, freq_buf, phase_buf, pos_buf=2, tag="scen_a")
    host.vm.load_program(host.program)

    n = int(duration_s * sr)
    BLOCK = cedar.BLOCK_SIZE
    n_blocks = (n + BLOCK - 1) // BLOCK
    pad = n_blocks * BLOCK - n
    pos_curve = np.linspace(0.0, 31.0, n + pad, dtype=np.float32)

    def push(block_idx):
        s = block_idx * BLOCK
        return pos_curve[s:s + BLOCK]

    output = render_blocks(host, n, set_pos_buffer=push)
    save_wav(os.path.join(OUT, "A_smooth_ramp.wav"), output, sr)
    return report("A", output, sr, freq_hz)


def scenario_b_ui_cadence(sr: int, freq_hz: float, duration_s: float,
                            ui_rate_hz: float = 60.0):
    """B: pos updated every ~16.7 ms via set_param (EnvMap slew engaged)."""
    print(f"Scenario B: UI-cadence emulation @ {ui_rate_hz:g} Hz steps")
    host = CedarTestHost(sr)
    bank_id = host.vm.wavetable_load_wav("morph", WAV_PATH)
    freq_buf  = host.set_param("freq",  freq_hz)
    phase_buf = host.set_param("phase",   0.0)
    pos_buf   = host.set_param("pos",     0.0)
    build_program(host, bank_id, freq_buf, phase_buf, pos_buf, tag="scen_b")
    host.vm.load_program(host.program)

    BLOCK = cedar.BLOCK_SIZE
    blocks_per_step = int(round((sr / ui_rate_hz) / BLOCK))
    if blocks_per_step < 1:
        blocks_per_step = 1
    n_blocks = int(duration_s * sr / BLOCK)
    # Drive pos as integer-quantized 0..31 stepping linearly across the run.
    # (Simulates a user dragging the slider — UI quantizes to integer values.)
    n_steps = n_blocks // blocks_per_step + 1

    def at(block_idx):
        if block_idx % blocks_per_step != 0:
            return None
        step_idx = block_idx // blocks_per_step
        # Sweep through 0..31 over the run (rounded).
        v = round(31.0 * step_idx / max(1, n_steps - 1))
        return float(min(31, max(0, v)))

    n_samples = n_blocks * BLOCK
    output = render_blocks(host, n_samples, set_param_at=at)
    save_wav(os.path.join(OUT, "B_ui_cadence.wav"), output, sr)
    return report("B", output, sr, freq_hz, ui_rate_hz)


def scenario_c_held_positions(sr: int, freq_hz: float, hold_s: float = 1.0):
    """C: pos held at 0, 7.5, 15.5, 23.5, 31 for hold_s each."""
    print("Scenario C: held positions baseline (no movement)")
    held_positions = [0.0, 7.5, 15.5, 23.5, 31.0]
    n_each = int(hold_s * sr)
    chunks = []
    for pos_val in held_positions:
        host = CedarTestHost(sr)
        bank_id = host.vm.wavetable_load_wav("morph", WAV_PATH)
        freq_buf  = host.set_param("freq",  freq_hz)
        phase_buf = host.set_param("phase",   0.0)
        pos_buf   = host.set_param("pos",     pos_val)
        build_program(host, bank_id, freq_buf, phase_buf, pos_buf, tag=f"scen_c_{pos_val}")
        host.vm.load_program(host.program)
        out = render_blocks(host, n_each)
        # Drop the first 0.05 s as a warmup / phase-init transient.
        chunks.append(out[int(0.05 * sr):])
    output = np.concatenate(chunks).astype(np.float32)
    save_wav(os.path.join(OUT, "C_held_positions.wav"), output, sr)
    return report("C", output, sr, freq_hz)


# ============================================================================
# Source-data inspection (no VM involved)
# ============================================================================


def inspect_bank_source(path: str) -> None:
    """Inspect the raw WAV contents — phase angles + RMS + cross-frame
    correlation — to confirm whether the source itself is the problem.
    """
    print(f"\nSource inspection: {path}")
    sr_, raw = wf.read(path)
    n_frames = len(raw) // N
    print(f"  {n_frames} frames, dtype={raw.dtype}")

    # Bin-1 phase + RMS per frame (after DC removal — what the preprocessor
    # actually sees).
    phases = np.zeros(n_frames)
    rmss   = np.zeros(n_frames)
    for i in range(n_frames):
        f = raw[i * N:(i + 1) * N].astype(np.float64)
        f -= f.mean()
        rmss[i] = float(np.sqrt(np.mean(f ** 2)))
        spec = np.fft.rfft(f)
        phases[i] = float(np.angle(spec[1]))

    phase_jumps = np.abs(np.diff(phases))
    print(f"  bin-1 phase: min={phases.min():.4f} max={phases.max():.4f} "
          f"max-adjacent-jump={phase_jumps.max():.4f}")
    print(f"  source RMS: min={rmss.min():.4f} max={rmss.max():.4f} "
          f"ratio={rmss.max() / max(rmss.min(), 1e-9):.2f}x")

    # Adjacent-frame correlation after DC removal — high = morph-friendly.
    print("  Adjacent-frame correlation (post DC removal):")
    correls = []
    for i in range(n_frames - 1):
        a = raw[i * N:(i + 1) * N].astype(np.float64)
        b = raw[(i + 1) * N:(i + 2) * N].astype(np.float64)
        a -= a.mean()
        b -= b.mean()
        c = float(np.corrcoef(a, b)[0, 1])
        correls.append(c)
    correls = np.array(correls)
    print(f"    min={correls.min():.4f} max={correls.max():.4f} mean={correls.mean():.4f}")


# ============================================================================
# Driver
# ============================================================================


def main():
    sr = 48000
    freq_hz = 220.0
    duration_s = 6.0
    ui_rate_hz = 60.0

    print(f"Output directory: {OUT}")
    print(f"Wavetable file:   {WAV_PATH}")
    print(f"Settings:         freq={freq_hz} Hz, sr={sr}, duration={duration_s}s\n")

    inspect_bank_source(WAV_PATH)
    print()

    a = scenario_a_smooth_ramp(sr, freq_hz, duration_s)
    b = scenario_b_ui_cadence(sr, freq_hz, duration_s, ui_rate_hz)
    c = scenario_c_held_positions(sr, freq_hz, hold_s=1.0)

    print("\n=== Attribution ===")
    if c["dist_max"] > 0:
        ratio_a = a["dist_max"] / c["dist_max"]
        ratio_b = b["dist_max"] / c["dist_max"]
        print(f"  spectral-dist peak ratio  A/C = {ratio_a:.1f}x   B/C = {ratio_b:.1f}x")
    if b["sideband_db"] is not None:
        if b["sideband_db"] > -60.0:
            print(f"  ⚠ Scenario B has UI-rate sidebands at {b['sideband_db']:+.1f} dB "
                  f"(target < -60 dB) — UI cadence is leaking into audio.")
        else:
            print(f"  ✓ Scenario B sidebands at {b['sideband_db']:+.1f} dB "
                  f"— UI cadence is well-suppressed.")

    print("\nWAVs saved to", OUT)
    print("Listen to:")
    print("  C_held_positions.wav — should be 5 distinct held tones, no zipper")
    print("  A_smooth_ramp.wav    — should be a smooth morph, no clicks/phasing")
    print("  B_ui_cadence.wav     — what the user hears in the web UI")


if __name__ == "__main__":
    main()

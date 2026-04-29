"""
Generate Serum-format wavetable WAV files
==========================================
Builds a small curated bank of wavetables and writes them to
`web/static/wavetables/` for the browser smoke test (PRD §11 Phase 4).

Format follows the Serum / Surge XT / Vital convention:
  - Mono WAV
  - 32-bit IEEE float
  - Total sample count = num_frames × 2048
  - Each consecutive 2048-sample chunk is one source frame
  - Sample rate is irrelevant (header value is cosmetic) — these are abstract
    waveform shapes, not time-domain audio.

The Cedar wavetable preprocessor (cedar/src/wavetable/preprocessor.cpp) does
all the band-limiting, fundamental-phase alignment, and RMS normalization
at load time, so the source frames here can be naive (and deliberately
band-unlimited — the saw and square banks have full-bandwidth harmonics
that the runtime mip pyramid will band-limit per-octave).

Usage
-----
    cd experiments
    uv run python generate_wavetables.py
"""

import os
import numpy as np
from scipy.io import wavfile

# Wavetable convention
N_SAMPLES = 2048

# Output to the web static dir so the browser can fetch these
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "..", "web", "static", "wavetables")
os.makedirs(OUT_DIR, exist_ok=True)


# ============================================================================
# Frame generators (each returns a (N_SAMPLES,) float32 array — one cycle)
# ============================================================================


def sine_frame() -> np.ndarray:
    t = np.arange(N_SAMPLES, dtype=np.float32) / N_SAMPLES
    return np.sin(2 * np.pi * t).astype(np.float32)


def triangle_frame() -> np.ndarray:
    t = np.arange(N_SAMPLES, dtype=np.float32) / N_SAMPLES
    return (2.0 * np.abs(2.0 * t - 1.0) - 1.0).astype(np.float32)


def saw_frame() -> np.ndarray:
    """Naive (band-unlimited) downward saw — preprocessor band-limits per mip."""
    t = np.arange(N_SAMPLES, dtype=np.float32) / N_SAMPLES
    return (1.0 - 2.0 * t).astype(np.float32)


def square_frame(duty: float = 0.5) -> np.ndarray:
    """Naive square — band-unlimited at the discontinuities."""
    t = np.arange(N_SAMPLES, dtype=np.float32) / N_SAMPLES
    return np.where(t < duty, 1.0, -1.0).astype(np.float32)


def harmonic_stack_frame(num_harmonics: int) -> np.ndarray:
    """Sum of the first num_harmonics harmonics with equal amplitude.

    Useful for testing how the runtime handles spectra with sharp harmonic
    boundaries — the mip pyramid should preserve all harmonics that fit
    under Nyquist at the playback frequency.
    """
    t = np.arange(N_SAMPLES, dtype=np.float64) / N_SAMPLES
    out = np.zeros(N_SAMPLES, dtype=np.float64)
    for k in range(1, num_harmonics + 1):
        out += np.sin(2 * np.pi * k * t) / k
    # Soft-normalize so peak ~ 0.95
    peak = float(np.max(np.abs(out)))
    if peak > 1e-9:
        out *= 0.95 / peak
    return out.astype(np.float32)


def formant_frame(f1: float, f2: float, f3: float) -> np.ndarray:
    """Crude vowel-like spectrum: 3 weighted harmonic clusters at f1/f2/f3.

    Frequencies are *harmonic indices* relative to the fundamental (1 = fund).
    """
    t = np.arange(N_SAMPLES, dtype=np.float64) / N_SAMPLES
    out = np.zeros(N_SAMPLES, dtype=np.float64)
    # Add a fundamental + clustered overtones around each formant index.
    out += 0.6 * np.sin(2 * np.pi * t)  # fundamental
    for f, gain in [(f1, 0.35), (f2, 0.30), (f3, 0.20)]:
        center = int(round(f))
        for h in range(max(1, center - 2), center + 3):
            out += gain * np.exp(-0.5 * ((h - f) / 1.0) ** 2) * np.sin(2 * np.pi * h * t)
    peak = float(np.max(np.abs(out)))
    if peak > 1e-9:
        out *= 0.95 / peak
    return out.astype(np.float32)


# ============================================================================
# Bank assemblers
# ============================================================================


def assemble(frames: list) -> np.ndarray:
    """Stack a list of (N_SAMPLES,) frames into one big mono buffer."""
    return np.concatenate(frames).astype(np.float32)


def basic_shapes_bank() -> np.ndarray:
    """4 frames: sine, triangle, saw, square."""
    return assemble([sine_frame(), triangle_frame(), saw_frame(), square_frame()])


def sine_to_saw_bank(num_frames: int = 32) -> np.ndarray:
    """Linear morph from sine to (band-unlimited) saw."""
    out = []
    a = sine_frame()
    b = saw_frame()
    for i in range(num_frames):
        morph = i / max(1, num_frames - 1)
        out.append((a * (1.0 - morph) + b * morph).astype(np.float32))
    return assemble(out)


def sine_to_square_bank(num_frames: int = 32) -> np.ndarray:
    """Linear morph from sine to square."""
    out = []
    a = sine_frame()
    b = square_frame()
    for i in range(num_frames):
        morph = i / max(1, num_frames - 1)
        out.append((a * (1.0 - morph) + b * morph).astype(np.float32))
    return assemble(out)


def harmonic_build_bank(num_frames: int = 32) -> np.ndarray:
    """Frame k = sum of first (k+1) harmonics. Frame 0 = pure sine, frame 31
    = saw-like with 32 harmonics.
    """
    return assemble([harmonic_stack_frame(i + 1) for i in range(num_frames)])


def pwm_sweep_bank(num_frames: int = 32) -> np.ndarray:
    """Square wave with PWM 5% → 95% across frames."""
    out = []
    for i in range(num_frames):
        duty = 0.05 + 0.90 * (i / max(1, num_frames - 1))
        out.append(square_frame(duty))
    return assemble(out)


def vowels_bank() -> np.ndarray:
    """5 vowel-like frames (a, e, i, o, u) for spectral-morph demos.

    Formant ratios are stylized, not anatomically accurate — they're chosen
    to give clearly different timbres when interpolated.
    """
    return assemble([
        formant_frame(8, 13, 25),    # 'a' (open)
        formant_frame(5, 22, 28),    # 'e'
        formant_frame(3, 25, 33),    # 'i' (bright, sharp)
        formant_frame(7, 11, 23),    # 'o'
        formant_frame(4, 9, 21),     # 'u' (dark)
    ])


# ============================================================================
# Driver
# ============================================================================


BANKS = [
    ("basic_shapes.wav",  basic_shapes_bank,    "4 frames: sine, triangle, saw, square"),
    ("sine_to_saw.wav",   sine_to_saw_bank,     "32-frame morph from sine to band-unlimited saw"),
    ("sine_to_square.wav", sine_to_square_bank, "32-frame morph from sine to square"),
    ("harmonic_build.wav", harmonic_build_bank, "32 frames: each adds one more harmonic"),
    ("pwm_sweep.wav",     pwm_sweep_bank,       "32 frames: square wave with 5%→95% PWM"),
    ("vowels.wav",        vowels_bank,          "5 frames: a/e/i/o/u-like vowel spectra"),
]


def write_bank(path: str, samples: np.ndarray):
    assert samples.dtype == np.float32, f"expected float32, got {samples.dtype}"
    assert len(samples) % N_SAMPLES == 0, (
        f"sample count {len(samples)} not a multiple of {N_SAMPLES}")
    # Sample rate is cosmetic in this format — 48000 keeps the WAV header
    # consistent with cedar's audio sample rate even though it's irrelevant.
    wavfile.write(path, 48000, samples)


def main():
    print(f"Writing wavetables to: {OUT_DIR}")
    print()

    for fname, builder, description in BANKS:
        out_path = os.path.join(OUT_DIR, fname)
        samples = builder()
        write_bank(out_path, samples)
        num_frames = len(samples) // N_SAMPLES
        size_kb = os.path.getsize(out_path) / 1024
        print(f"  {fname:24s}  {num_frames:3d} frames  {size_kb:6.1f} KB  ({description})")

    # Round-trip sanity check via cedar_core
    print()
    print("Round-trip check: load each bank into cedar_core and confirm preprocessing succeeds...")
    try:
        import cedar_core as cedar
        vm = cedar.VM()
        for fname, _builder, _ in BANKS:
            path = os.path.join(OUT_DIR, fname)
            try:
                vm.wavetable_load_wav(fname.removesuffix(".wav"), path)
                ok = vm.wavetable_has(fname.removesuffix(".wav"))
                status = "OK" if ok else "loaded but registry says missing"
                print(f"  {fname:24s} -> {status}")
            except Exception as e:
                print(f"  {fname:24s} -> FAILED: {e}")
    except ImportError:
        print("  (cedar_core not built — skipping round-trip check)")


if __name__ == "__main__":
    main()

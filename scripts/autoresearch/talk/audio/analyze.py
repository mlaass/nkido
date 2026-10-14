"""Before/after render + spectral comparison for the non-bit-identical winners.

usage: analyze.py   (run from build/talk-slides/audio)
Renders each patch with the master build (before) and the winner build (after),
prints the null-test level and writes <case>.png (spectra + difference spectrum)
in the deck's colours, plus 16-bit WAVs for listening.
"""
import subprocess
import numpy as np
import scipy.io.wavfile as wav
import scipy.signal as sig
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

W = "/home/moritz/workspace/nkido/build/talk-slides"
BEFORE = f"{W}/wt-master/build-rel/bin/nkido"
CASES = [  # (case, patch, winner build, label)
    ("distort", "distort.akk", f"{W}/wt-opus-distort-202609251739/build-rel/bin/nkido",
     "saturate (tanh): opus, 30.7x faster"),
    ("svf", "svf.akk", f"{W}/wt-sonnet-svf-202609261247/build-rel/bin/nkido",
     "lp (state-variable filter): sonnet, 9.2x faster"),
]
BG, FG, AMBER, GRAY, MUTED = "#212121", "#D9D9D9", "#F1C232", "#8C8C8C", "#6B6B6B"
SECONDS = 8


def render(binary, patch, out):
    subprocess.run([binary, "render", patch, "-o", out, "--seconds", str(SECONDS), "--float32"],
                   check=True, capture_output=True)
    sr, x = wav.read(out)
    return sr, x.astype(np.float64)


def db(x):
    return 20 * np.log10(np.maximum(x, 1e-12))


def psd(x, sr):
    f, p = sig.welch(x, sr, nperseg=2048)
    return f, 10 * np.log10(np.maximum(p, 1e-30))


for case, patch, after_bin, label in CASES:
    sr, a = render(BEFORE, patch, f"{case}_before.wav")
    _, b = render(after_bin, patch, f"{case}_after.wav")
    a, b = (a.mean(axis=1), b.mean(axis=1)) if a.ndim == 2 else (a, b)
    d = b - a
    peak, rms = db(np.max(np.abs(d))), db(np.sqrt(np.mean(d ** 2)))
    sig_rms = db(np.sqrt(np.mean(a ** 2)))
    print(f"{case}: signal rms {sig_rms:.1f} dBFS, diff peak {peak:.1f} dBFS, diff rms {rms:.1f} dBFS, "
          f"bit-identical samples {np.mean(d == 0) * 100:.1f}%")
    for name, x in (("before", a), ("after", b)):
        wav.write(f"{case}_{name}_16bit.wav", sr, (np.clip(x, -1, 1) * 32767).astype(np.int16))

    f, pa = psd(a, sr)
    _, pb = psd(b, sr)
    _, pd = psd(d, sr) if np.any(d) else (f, np.full_like(pa, -300))
    fig, ax = plt.subplots(figsize=(8, 4.2), dpi=250, facecolor=BG)
    ax.set_facecolor(BG)
    ax.semilogx(f[1:], pa[1:], color=GRAY, lw=2.2, label="before (original code)")
    ax.semilogx(f[1:], pb[1:], color=AMBER, lw=1.0, label="after (agent's code)")
    ax.semilogx(f[1:], pd[1:], color="#E06C5A", lw=1.0, label="difference")
    ax.set_xlim(20, sr / 2)
    ax.set_ylim(max(-300, np.percentile(pd[1:], 1) - 10), pa[1:].max() + 10)
    ax.set_xlabel("Hz", color=FG)
    ax.set_ylabel("power (dB)", color=FG)
    ax.set_title(f"{label}\ndifference peak {peak:.0f} dBFS, rms {rms:.0f} dBFS", color=FG, fontsize=11)
    ax.tick_params(colors=FG)
    for s in ax.spines.values():
        s.set_color(MUTED)
    ax.grid(color="#333333", lw=0.5)
    ax.legend(facecolor=BG, edgecolor=MUTED, labelcolor=FG, fontsize=9, loc="lower left")
    fig.tight_layout()
    fig.savefig(f"{case}.png", facecolor=BG)
    plt.close(fig)

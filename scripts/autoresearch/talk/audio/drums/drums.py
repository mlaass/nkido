"""Drum-loop before/after renders for the two non-bit-identical winners.

usage: uv run --project <repo>/experiments python drums.py   (from this directory)
Loops in loops/ (Orange Free Sounds, CC BY 4.0 -- credit orangefreesounds.com):
Amen break, acoustic old-school hip-hop (100 bpm), rock (122 bpm).
Each loop x 2 effects (saturate = opus tanh, lp sweep = sonnet svf) is rendered with
master (before) and the winner build (after); prints the null-test level, writes 16-bit WAVs.
One cycle = one loop, so the loop retriggers exactly at its own length.
"""
import os
import subprocess
import numpy as np
import scipy.io.wavfile as wav
import scipy.signal as sig
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

W = "/home/moritz/workspace/nkido/build/talk-slides"
BEFORE = f"{W}/wt-master/build-rel/bin/nkido"
AFTER = {"distort": f"{W}/wt-opus-distort-202609251739/build-rel/bin/nkido",
         "svf": f"{W}/wt-sonnet-svf-202609261247/build-rel/bin/nkido"}
BANK = "file://" + os.path.abspath("loops/strudel.json")
LOOPS = {"amen": (6.9708, 2), "hiphop": (2.4000, 4), "rock": (7.8689, 2)}  # seconds, repeats
# Basslines from the welcome patches (bass voice only, their own filter/saturation removed).
BASS = {"acid303": (130, 'n"a2 a3 c3 a2 e3 a2 g3 c3".fast(4)\n    |> saw(@freq / 2) * ar(@trig, 0.001, 0.2) * 0.6', 8),
        "rockbass": (120, 'n"d2 [d2 <a1 [f#3 e3]>] [~ < ~ e3 >] ~"\n    |> tri(@freq) * ar(@trig, 0.1, 0.1) * 0.8', 8)}
FX = {"distort": "saturate(@, 4)",
      "svf": "lp(@, sine(0.25) * 2200 + 2500, 1.8)"}


def patch(name, dur, fx):
    return (f'samples("{BANK}")\nbpm = {60 / dur:.4f}\n'
            f's"{name}".bank("loops") * 0.7\n    |> {fx}\n    |> out(@)\n')


def render(binary, src, out, seconds):
    subprocess.run([binary, "render", src, "-o", out, "--seconds", str(seconds), "--float32"],
                   check=True, capture_output=True)
    return wav.read(out)


DIFF_GAIN_DB = 100  # the difference is ~-115 dBFS: +100 dB makes it audible
BG, FG, AMBER, GRAY, MUTED = "#212121", "#D9D9D9", "#F1C232", "#8C8C8C", "#6B6B6B"


def spectrum_png(case, a, b, d, sr, peak, rms):
    def psd(x):
        f, p = sig.welch(x, sr, nperseg=2048)
        return f[1:], 10 * np.log10(np.maximum(p[1:], 1e-30))
    f, pa = psd(a.mean(axis=1))
    _, pb = psd(b.mean(axis=1))
    _, pd = psd(d.mean(axis=1))
    fig, ax = plt.subplots(figsize=(8, 4.2), dpi=250, facecolor=BG)
    ax.set_facecolor(BG)
    ax.semilogx(f, pa, color=GRAY, lw=2.2, label="before (original code)")
    ax.semilogx(f, pb, color=AMBER, lw=1.0, label="after (agent's code)")
    ax.semilogx(f, pd, color="#E06C5A", lw=1.0, label="difference")
    ax.set_xlim(20, sr / 2)
    ax.set_ylim(np.percentile(pd, 1) - 10, pa.max() + 10)
    ax.set_xlabel("Hz", color=FG)
    ax.set_ylabel("power (dB)", color=FG)
    ax.set_title(f"{case}: difference peak {peak:.0f} dBFS, rms {rms:.0f} dBFS", color=FG, fontsize=11)
    ax.tick_params(colors=FG)
    for s in ax.spines.values():
        s.set_color(MUTED)
    ax.grid(color="#333333", lw=0.5)
    ax.legend(facecolor=BG, edgecolor=MUTED, labelcolor=FG, fontsize=9, loc="lower left")
    fig.tight_layout()
    fig.savefig(f"{case}.png", facecolor=BG)
    plt.close(fig)


def db(x):
    return 20 * np.log10(max(x, 1e-12))


CASES = [(f"{n}_{fx}", patch(n, dur, e), round(dur * reps, 3), fx)
         for n, (dur, reps) in LOOPS.items() for fx, e in FX.items()]
CASES += [(f"{n}_{fx}", f"bpm = {bpm}\n{src}\n    |> {e}\n    |> out(@)\n", secs, fx)
          for n, (bpm, src, secs) in BASS.items() for fx, e in FX.items()]

for case, src, secs, fx in CASES:
    open(f"{case}.akk", "w").write(src)
    sr, a = render(BEFORE, f"{case}.akk", f"/dev/shm/{case}_b.wav", secs)
    _, b = render(AFTER[fx], f"{case}.akk", f"/dev/shm/{case}_a.wav", secs)
    assert np.abs(a).max() > 0.01, f"{case}: silent render"
    d = b.astype(np.float64) - a
    print(f"{case:15s} signal peak {db(np.abs(a).max()):6.1f} dBFS  diff peak {db(np.abs(d).max()):7.1f} dBFS  "
          f"diff rms {db(np.sqrt(np.mean(d ** 2))):7.1f} dBFS")
    peak, rms = db(np.abs(d).max()), db(np.sqrt(np.mean(d ** 2)))
    spectrum_png(case, a, b, d, sr, peak, rms)
    wav.write(f"{case}_diff{DIFF_GAIN_DB}.wav", sr,
              (np.clip(d.mean(axis=1) * 10 ** (DIFF_GAIN_DB / 20), -1, 1) * 32767).astype(np.int16))
    for tag, x in (("before", a), ("after", b)):
        wav.write(f"{case}_{tag}.wav", sr, (np.clip(x, -1, 1) * 32767).astype(np.int16))

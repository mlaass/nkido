"""Per-algorithm slides, two deep dives, spectra placeholder slide.

usage: gen3.py REVISION -> prints {"requests": [...], "writeControl": {...}}
Inserted after the results slide (n25); the old accepted-example slide (n26) is replaced by
the opus-distort deep dive. Final order of the experiment block:
m01 motivation, n23 loop, n25 results, a01..a05 algorithms, d01 d02 deep dives,
s01 spectra, n27 harness catch, m02 lessons.
"""
import json, subprocess, sys, tempfile
import gen
from gen import text, card, foot, table, AMBER, TXT, MUTED, Y0, L, HELPER

ALGOS = [  # id, title, big number, sub lines, steps
    ("a01", "Add, sub, mul are memory-bound: the best model reached 1.45x",
     "1.45x", "nemotron-3-ultra (free)\nbit-identical",
     ["AVX2 kernels, scalar loop kept as the exact fallback: 1.34x",
      "__restrict, alignment hints and unroll pragmas: 1.45x",
      "6 further attempts (2x to 16x unrolling) found nothing",
      "Every model stopped between 1.27x and 1.45x here"]),
    ("a02", "Distortion: opus replaced libm's tanh with a vector approximation, 31x faster",
     "30.7x", "tanh (soft clip 7.9x)\nopus\n-115 dBFS vs original",
     ["AVX2 kernel with an exp-based tanh approximation: 10.5x",
      "Shorter rational and exponent forms, fewer ops per 8 samples: 21.5x",
      "Skip the dry/wet mix at its default: 24.7x",
      "2^(j/8) lookup tables replace polynomial terms: 30.7x"]),
    ("a03", "Formant filter: opus kept every sample identical and got 8.1x faster",
     "8.13x", "opus\nbit-identical",
     ["glibc's tanf polynomial reproduced exactly in AVX2: 4.4x",
      "Coefficients computed a chunk ahead to fill recurrence stalls: 5.2x",
      "Cheaper exact clamp, NaN handling off the critical path: 6.3x",
      "Per-chunk NaN flag instead of per-sample checks: 8.1x"]),
    ("a04", "State-variable filter: sonnet rewrote the recurrence as a matrix step, 9.2x faster",
     "9.2x", "lp (hp 8.1x, bp 8.9x)\nsonnet\n-104 dBFS vs original",
     ["Vector tan approximation for the whole block's coefficients: 2.6x",
      "Recurrence as a precomputed 2x2 transition: 3.3x",
      "Clamp off the critical path, block redone on overflow: 4.9x",
      "Two samples per step, FMA, half the coefficient work: 9.2x"]),
    ("a05", "Freeverb: sonnet ran the 16 comb filters as AVX2 lanes, 7.3x faster",
     "7.25x", "sonnet\nbit-identical",
     ["Batched delay-line access, 16 combs as AVX2 lanes: 3.4x",
      "In place, no staging copies or 8x8 transposes: 5.7x",
      "Vectorised parameter setup with exact clamp semantics: 7.3x",
      "5 later attempts (prefetch, specialising, fusing) were all slower"]),
]

DEEP = [
    ("d01", "Deep dive, opus on tanh: each step removed the bottleneck the last one exposed",
     [["#", "Bottleneck the agent measured", "Change", "tanh"],
      ["1", "scalar libm tanh per sample", "AVX2 kernel, exp-based approximation", "10.5x"],
      ["2", "~40 vector ops per 8 samples", "rational minimax x*P(x^2)/Q(x^2)", "18.7x"],
      ["3", "~23 ops including a divide", "1 - 2/(1 + e^2y), degree-4 2^f", "21.5x"],
      ["4", "dry/wet mix: 3 of ~24 ops", "skip the mix at default dry/wet", "24.7x"],
      ["5", "~20 FP-port ops", "8-entry 2^(j/8) lookup table", "26.5x"],
      ["6", "abs/sign split around the clamp", "evaluate signed, two-sided clamp", "28.7x"],
      ["7", "soft clip: two divides", "skip divides when all lanes saturate", "28.9x"],
      ["8", "~14 ops", "(2^f - 2^-k/8) / (2^f + 2^-k/8)", "30.7x"]],
     "8 of 8 accepted, 2 to 7 min and $0.23 to $1.16 (API-equivalent) per step. Output within -115 dBFS."),
    ("d02", "Deep dive, opus on the formant filter: 8.1x faster, every sample identical",
     [["#", "Bottleneck the agent measured", "Change", "Result"],
      ["1", "3 scalar tanf calls per sample", "glibc's tanf reproduced exactly in AVX2", "4.4x"],
      ["2", "recurrence waits on the coefficient pass", "coefficients one chunk ahead", "5.2x"],
      ["3", "shuffles and sums inside the loop", "prebuilt inputs, sum after the loop", "5.8x"],
      ["4", "4-op clamp on the critical path", "2-op bit-pattern clamp, NaN off-path", "6.3x"],
      ["5", "port pressure next to the recurrence", "min/max clamps, mix folded in", "6.7x"],
      ["6", "FP ports saturated", "coefficient stages between samples", "7.1x"],
      ["7", "per-sample NaN checks", "per-chunk NaN flag", "rejected"],
      ["8", "same, plus redundant ops", "NaN flag and a dropped constant column", "8.1x"]],
     "Step 1: a tan approximation failed the equality gate first, because the resonant filter amplified "
     "the error. Step 7: faster in the quick check, below 5% in the full benchmark."),
]


def algo_elements(sid, big, sub, steps):
    return [card(f"{sid}k", 0.5, Y0, 2.8, 3.1),
            text(f"{sid}n", 0.7, Y0 + 0.2, 2.4, 0.95, big, 44, AMBER, bold=True),
            text(f"{sid}s", 0.7, Y0 + 1.25, 2.4, 1.6, sub, 16, TXT, space_below=6),
            text(f"{sid}h", 3.65, Y0, 5.85, 0.45, "What it did", 18, AMBER, bold=True),
            text(f"{sid}t", 3.65, Y0 + 0.55, 5.85, 2.55, "\n".join(steps), 16, TXT, space_below=10)]


def main():
    rev = sys.argv[1]
    reqs = [{"deleteObject": {"objectId": "n26_s"}}]
    # after the delete: ... m01 24, n23 25, n25 26, n27 27, m02 28, thank you 29
    order = [a[0] for a in ALGOS] + [d[0] for d in DEEP] + ["s01"]
    titles = {a[0]: a[1] for a in ALGOS} | {d[0]: d[1] for d in DEEP} | {
        "s01": "Where the output changed, the difference peaks at -115 dBFS, below the 16-bit floor"}
    for k, sid in enumerate(order):
        reqs.append({"createSlide": {"objectId": f"{sid}_s", "insertionIndex": 27 + k,
                     "slideLayoutReference": {"layoutId": L["TITLE_ONLY"]},
                     "placeholderIdMappings": [{"layoutPlaceholder": {"type": "TITLE", "index": 0},
                                                "objectId": f"{sid}_t"}]}})
        reqs.append({"insertText": {"objectId": f"{sid}_t", "text": titles[sid]}})
        reqs += gen.title_style(sid, "TITLE_ONLY")
        reqs.append({"updatePageElementTransform": {"objectId": f"{sid}_t", "applyMode": "ABSOLUTE",
                     "transform": {"scaleX": 2.6213, "scaleY": 0.1909, "shearX": 0, "shearY": 0,
                                   "translateX": 311700, "translateY": 445025, "unit": "EMU"}}})
        reqs.append({"updateTextStyle": {"objectId": f"{sid}_t", "textRange": {"type": "ALL"},
                     "style": {"fontSize": {"magnitude": 22, "unit": "PT"}}, "fields": "fontSize"}})
    spec = []
    for sid, _, big, sub, steps in ALGOS:
        spec.append({"id": f"{sid}_s", "existing": True, "elements": algo_elements(sid, big, sub, steps)})
    for sid, _, rows, note in DEEP:
        spec.append({"id": f"{sid}_s", "existing": True, "elements": [
            table(f"{sid}x", Y0 - 0.05, rows, [0.45, 3.65, 3.9, 1.0], ["START", "START", "START", "END"], 13),
            text(f"{sid}f", 0.5, 4.68, 8.3, 0.62, note, 11, MUTED)]})
    spec.append({"id": "s01_s", "existing": True, "elements": [
        card("s01a", 0.5, Y0, 4.4, 2.3, fill="#2A2A2A"),
        text("s01ap", 0.65, Y0 + 0.9, 4.1, 0.5, "[spectrum: distort / tanh]", 14, MUTED, align="CENTER"),
        card("s01b", 5.1, Y0, 4.4, 2.3, fill="#2A2A2A"),
        text("s01bp", 5.25, Y0 + 0.9, 4.1, 0.5, "[spectrum: svf / lp]", 14, MUTED, align="CENTER"),
        text("s01c", 0.5, Y0 + 2.5, 9.0, 0.9,
             "Same patch rendered with the original code (grey) and the agent's code (amber); the red "
             "line is the difference. Distortion: peak -115 dBFS. Filter sweep: peak -127 dBFS.",
             15, TXT)]})
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, dir=".") as f:
        json.dump({"slides": [{**s, "elements": [{**e, "id": f"e_{e['id']}"} for e in s["elements"]]}
                              for s in spec]}, f)
    r = subprocess.run([sys.executable, HELPER, "build", f.name, "--revision", rev],
                       capture_output=True, text=True)
    sys.stderr.write(r.stderr)
    reqs += json.loads(r.stdout)["requests"]

    def rnd(o):
        if isinstance(o, float): return round(o, 3)
        if isinstance(o, dict): return {k: rnd(v) for k, v in o.items()}
        if isinstance(o, list): return [rnd(v) for v in o]
        return o
    reqs = [q for q in rnd(reqs) if not ("updateParagraphStyle" in q and
            q["updateParagraphStyle"].get("style") == {"alignment": "START"})]
    amber = {"opaqueColor": {"rgbColor": {"red": 0.945, "green": 0.761, "blue": 0.196}}}
    for sid, _, rows, _ in DEEP:  # result column in amber
        for i in range(1, len(rows)):
            reqs.append({"updateTextStyle": {"objectId": f"e_{sid}x", "cellLocation": {"rowIndex": i,
                         "columnIndex": 3}, "textRange": {"type": "ALL"},
                         "style": {"bold": True, "foregroundColor": amber}, "fields": "bold,foregroundColor"}})
    print(json.dumps({"requests": reqs, "writeControl": {"requiredRevisionId": rev}},
                     ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main()

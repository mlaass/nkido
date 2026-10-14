"""Cut the added slides down to the experiment section: motivation, 4 experiment slides, lessons.

usage: gen2.py REVISION  -> prints {"requests": [...], "writeControl": {...}}
"""
import json, subprocess, sys, tempfile
import gen
from gen import text, card, foot, AMBER, TXT, MUTED, Y0, L, HELPER

KEEP = {"n23", "n25", "n26", "n27"}
DROP = [f"n{i:02d}" for i in range(1, 31) if f"n{i:02d}" not in KEEP]


def stat_cards(prefix, y, items, h=1.5):
    w, gap, out = (9.0 - 0.3 * 2) / 3, 0.3, []
    for k, (num, lab, hi) in enumerate(items):
        x = 0.5 + k * (w + gap)
        out += [card(f"{prefix}{k}c", x, y, w, h),
                text(f"{prefix}{k}n", x + 0.15, y + 0.08, w - 0.3, 0.72, num, 28, AMBER if hi else TXT, bold=True),
                text(f"{prefix}{k}l", x + 0.15, y + 0.78, w - 0.3, 0.68, lab, 14, TXT)]
    return out


def lesson_rows(prefix, y, rows, row_h=0.95):
    out = []
    for k, (lead, body) in enumerate(rows):
        yy = y + k * row_h
        out += [text(f"{prefix}{k}h", 0.5, yy, 2.9, row_h - 0.05, lead, 16, AMBER, bold=True),
                text(f"{prefix}{k}b", 3.5, yy, 6.0, row_h - 0.05, body, 15, TXT)]
    return out


def main():
    rev = sys.argv[1]
    reqs = [{"deleteObject": {"objectId": f"{s}_s"}} for s in DROP]
    # results slide: setup goes into the footnote (the setup slide is gone)
    reqs += [{"deleteObject": {"objectId": "e_n25f"}}, {"deleteObject": {"objectId": "e_n27x"}}]
    # new slides: motivation before the loop slide, lessons after the harness-catch slide
    for sid, idx in (("m01", 24), ("m02", 29)):
        reqs.append({"createSlide": {"objectId": f"{sid}_s", "insertionIndex": idx,
                     "slideLayoutReference": {"layoutId": L["TITLE_ONLY"]},
                     "placeholderIdMappings": [{"layoutPlaceholder": {"type": "TITLE", "index": 0},
                                                "objectId": f"{sid}_t"}]}})
    titles = {"m01": "Every opcode is a small, measurable kernel: a natural target for an agent",
              "m02": "Lessons learned"}
    for sid, t in titles.items():
        reqs.append({"insertText": {"objectId": f"{sid}_t", "text": t}})
        reqs += gen.title_style(sid, "TITLE_ONLY")
        reqs.append({"updatePageElementTransform": {"objectId": f"{sid}_t", "applyMode": "ABSOLUTE",
                     "transform": {"scaleX": 2.6213, "scaleY": 0.1909, "shearX": 0, "shearY": 0,
                                   "translateX": 311700, "translateY": 445025, "unit": "EMU"}}})
        reqs.append({"updateTextStyle": {"objectId": f"{sid}_t", "textRange": {"type": "ALL"},
                     "style": {"fontSize": {"magnitude": 22, "unit": "PT"}}, "fields": "fontSize"}})
    spec = {"slides": [
        {"id": "m01_s", "existing": True, "elements":
            gen.flow("m01f", Y0, 2.0, [
                ("One opcode", "a 128-sample block in, a block out; its state lives in a pool, "
                               "not in the kernel. 155 opcodes share this shape."),
                ("What the harness already has", "the original code as an exact reference, "
                                                 "a benchmark, and a DSP experiment script")], size=15)
            + [text("m01q", 0.5, Y0 + 2.3, 9.0, 1.0,
                    "Can an agent make them faster without changing the sound?", 22, AMBER, bold=True)]},
        {"id": "n25_s", "existing": True, "elements": [
            text("n25g", 0.5, 4.85, 8.3, 0.45,
                 "12 models, 5 DSP targets, 8 iterations per run, 311 attempts. Best accepted speedup vs the "
                 "original scalar code, geometric mean over the target's opcodes. *3 of 8 iterations.",
                 10, MUTED)]},
        {"id": "n27_s", "existing": True, "elements":
            [text("n27y", 0.5, 2.95, 9.0, 0.5,
                  "gpt-5.6-sol on the formant filter. Limit -100 dBFS; every final verify draws a new random seed.",
                  14, MUTED)]
            + stat_cards("n27s", 3.5, [("311", "attempts, all models", False),
                                       ("111", "accepted: faster, output unchanged", False),
                                       ("9", "faster but the audio changed: all rejected", True)])},
        {"id": "m02_s", "existing": True, "elements": lesson_rows("m02r", Y0 - 0.05, [
            ("The gates decide, not the agent",
             "9 patches were faster but changed the audio. The agent's own check passed at least 5 of them; "
             "hidden random inputs caught all 9."),
            ("The target matters more than the model",
             "Every model stopped at 1.27 to 1.45x on arith. On distort the best results ranged from 5x to 16x."),
            ("Price does not predict results",
             "sonnet: 26 accepts for $17. opus: 25 for $41 (API-equivalent). glm-5.3: 5 for $22."),
            ("Quotas break unattended runs",
             "Credit limits, rate limits and usage caps turned runs into instant failures. The loop has to "
             "stop on access errors, or it records junk.")])},
    ]}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, dir=".") as f:
        json.dump({"slides": [{**s, "elements": [{**e, "id": f"e_{e['id']}"} for e in s["elements"]]}
                              for s in spec["slides"]]}, f)
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
    print(json.dumps({"requests": reqs, "writeControl": {"requiredRevisionId": rev}},
                     ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main()

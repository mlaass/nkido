"""Generate Google Slides batchUpdate requests for the ADCx Gather 2026 slides.

usage: gen.py REVISION BATCH   -> prints {"requests": [...], "writeControl": {...}}
New slides use the deck's own layouts (TITLE, SECTION_HEADER, TITLE_ONLY, MAIN_POINT,
BIG_NUMBER) so they inherit its background, title style and logo; tables, cards and
flows are built with the google-workspace skill's slides_helper.
"""
import json, subprocess, sys, tempfile

HELPER = ("/home/moritz/.claude/skills/synced/bfa15073-c38a-4300-a363-999066653253_"
          "0db87bbb-3a12-43e2-813c-23a9b5493ef3/google-workspace/scripts/slides_helper.py")
L = {"TITLE": "g3a8b88a45b2_0_26", "SECTION_HEADER": "g3a8b88a45b2_0_30",
     "TITLE_ONLY": "g3a8b88a45b2_0_42", "MAIN_POINT": "g3a8b88a45b2_0_49",
     "BIG_NUMBER": "g3a8b88a45b2_0_61"}
PH = {"TITLE": [("CENTERED_TITLE", 0, "t"), ("SUBTITLE", 0, "b")],
      "SECTION_HEADER": [("TITLE", 0, "t")], "TITLE_ONLY": [("TITLE", 0, "t")],
      "MAIN_POINT": [("TITLE", 0, "t")], "BIG_NUMBER": [("TITLE", 0, "t"), ("BODY", 0, "b")]}
TXT, MUTED, AMBER, CARD = "#D9D9D9", "#9E9E9E", "#F1C232", "#2E2E2E"
MONO, SANS = "Roboto Mono", "Arial"
Y0 = 1.5  # content top: leaves room for a two-line title


def text(i, x, y, w, h, s, size=16, color=TXT, **kw):
    return {"id": i, "type": "text", "x": x, "y": y, "w": w, "h": h, "text": s,
            "size": size, "color": color, "font": kw.pop("font", SANS), **kw}


def card(i, x, y, w, h, fill=CARD):
    return {"id": i, "type": "round_rect", "x": x, "y": y, "w": w, "h": h, "fill": fill}


def code(i, x, y, w, h, s, size=16):
    return [card(i + "c", x, y, w, h),
            text(i, x + 0.15, y + 0.12, w - 0.3, h - 0.24, s, size, TXT, font=MONO)]


def foot(i, s):
    return text(i, 0.5, 5.07, 8.3, 0.4, s, 10, MUTED)


def table(i, y, rows, widths, align, size=14, h=None):
    t = {"id": i, "type": "table", "x": 0.5, "y": y, "w": sum(widths), "rows": rows,
         "header": True, "size": size, "col_widths": widths, "col_align": align,
         "color": TXT, "font": SANS}
    return t


def flow(prefix, y, h, boxes, gap=0.25, x0=0.5, width=9.0, size=14, hi=None):
    n = len(boxes); w = (width - gap * (n - 1)) / n; out = []
    for k, (head, body) in enumerate(boxes):
        x = x0 + k * (w + gap)
        out.append(card(f"{prefix}{k}c", x, y, w, h))
        out.append(text(f"{prefix}{k}h", x + 0.12, y + 0.12, w - 0.24, 0.45, head, size + 2,
                        AMBER if hi is None or k == hi else TXT, bold=True))
        out.append(text(f"{prefix}{k}b", x + 0.12, y + 0.6, w - 0.24, h - 0.72, body, size, TXT))
    return out


def bars(prefix, y, items, maxw=5.6, label_w=2.6, row=0.52, hi=None):
    out, top = [], max(v for _, v in items)
    for k, (lab, v) in enumerate(items):
        yy = y + k * row
        col = AMBER if k == hi else "#6B6B6B"
        out.append(text(f"{prefix}{k}l", 0.5, yy, label_w, row - 0.08, lab, 16, TXT))
        out.append({"id": f"{prefix}{k}r", "type": "rect", "x": 0.5 + label_w, "y": yy + 0.06,
                    "w": max(0.05, maxw * v / top), "h": row - 0.2, "fill": col})
        out.append(text(f"{prefix}{k}v", 0.5 + label_w + maxw * v / top + 0.1, yy, 0.8,
                        row - 0.08, str(v), 16, AMBER if k == hi else TXT, bold=True))
    return out


# (id, layout, {placeholder role: text}, [helper elements])
SLIDES = [
 # ---------------------------------------------------------------- batch 1: opening, language, runtime
 ("n01", "TITLE", {"t": "Compiling Music",
   "b": "Live coding with a zero-allocation C++20 VM\nMoritz Laass @ ADCx Gather 2026"}, []),
 ("n02", "MAIN_POINT", {"t": "A live-coding language and a real-time C++ engine, built largely by AI. "
   "This talk is about why I trust it."}, []),
 ("n03", "TITLE_ONLY", {"t": "The C++ tests are almost as large as the engine they test"},
  [x for k, (num, lab, hi) in enumerate([
      ("72.9k", "lines of C++: engine and compiler", True),
      ("64.9k", "lines of C++ tests", True),
      ("28.7k", "lines of TypeScript and Svelte: web IDE", False),
      ("155", "DSP opcodes", False),
      ("87", "per-opcode DSP verification scripts", False),
      ("919", "commits since January 2026", False)])
   for x in (lambda cx, cy: [card(f"n03k{k}", cx, cy, 2.8, 1.65),
       text(f"n03n{k}", cx + 0.15, cy + 0.1, 2.5, 0.8, num, 36, AMBER if hi else TXT, bold=True),
       text(f"n03l{k}", cx + 0.15, cy + 0.92, 2.5, 0.68, lab, 14, TXT)])(
           0.5 + (k % 3) * 3.1, Y0 + (k // 3) * 1.8)]
  + [foot("n03f", "Counted from the git tree on 28 Sep 2026; generated files excluded.")]),
 ("n04", "SECTION_HEADER", {"t": "Demo: one patch, built live in the browser IDE"}, []),
 ("n05", "SECTION_HEADER", {"t": "Language"}, []),
 ("n06", "TITLE_ONLY", {"t": "Pipes read left to right, and @ is the signal so far"},
  code("n06a", 0.5, Y0, 5.2, 1.45, "saw(110) * 0.3\n    |> lp(@, cutoff, 1.8)\n    |> out(@)")
  + code("n06b", 0.5, Y0 + 1.7, 5.2, 1.75,
         'n"c4 e4 g4" as e\n    |> sine(e.freq)\n    |> @ * e.vel\n    |> out(@)')
  + [text("n06t", 6.0, Y0, 3.5, 3.4,
          "|> passes the signal to the next stage\n@ is the hole where it goes in\n"
          "as names a stage so later stages can read its fields", 18, TXT, space_below=10)]),
 ("n07", "TITLE_ONLY", {"t": "Mini-notation is a real parser inside the string"},
  [table("n07t", Y0, [["Pattern", "What it plays"],
     ['n"c4 e4 g4"', "one note per cycle (top level alternates)"],
     ['n"[c4 e4 g4 b4]"', "four notes inside one cycle"],
     ['s"bd(3,8)"', "Euclidean rhythm: 3 hits over 8 steps"],
     ['chord("Cmaj7 Am7 <Gm5 G7>")', "chords; <...> alternates per cycle"]],
     [3.9, 5.1], ["START", "START"], 15),
   text("n07x", 0.5, 4.35, 9, 0.6, "Every event is a record: .freq .vel .trig .gate .note .dur",
        18, AMBER)]),
 ("n08", "TITLE_ONLY", {"t": "Types coerce and warn, so a typo mid-set does not stop the music"},
  [text("n08t", 0.5, Y0, 5.0, 3.3,
        "A void argument becomes silence instead of a compile error\n"
        "stereo() on a signal that is already stereo passes it through\n"
        "The cost: output can be quietly wrong, so warnings stay visible in the editor",
        18, TXT, space_below=12)]
  + code("n08c", 5.8, Y0, 3.7, 1.6,
         "warning[W182]:\nstereo() called on a\nstereo signal: returning\nthe input unchanged", 14)),
 ("n09", "TITLE_ONLY", {"t": "One name, many forms: the compiler picks the overload by argument shape"},
  code("n09a", 0.5, Y0, 9.0, 1.2,
       "saw(220) |> stereo(@) |> pingpong(@, 0.25, 0.4)      // one stereo signal\n"
       "pingpong(saw(220), saw(221), 0.25, 0.4, 1.0)         // left, right, width", 15)
  + [text("n09t", 0.5, Y0 + 1.5, 9.0, 1.0,
          "The same resolution applies to builtins and to user-defined functions.", 18, TXT)]),
 ("n10", "SECTION_HEADER", {"t": "Runtime"}, []),
 ("n11", "TITLE_ONLY", {"t": "Every stateful node keeps its state through a stable semantic ID"},
  flow("n11f", Y0, 1.9, [("Source path", "main/track1/osc\nwhere the node sits in the program"),
                         ("FNV-1a hash", "the same path gives the same ID after every edit"),
                         ("State pool", "phase, filter memory, delay lines, all pre-allocated")])
  + [text("n11t", 0.5, Y0 + 2.2, 9.0, 0.9,
          "Instructions are fixed-width and address buffers by index. The audio path never allocates.",
          18, TXT)]),
 ("n12", "TITLE_ONLY", {"t": "Hot-swap runs the old and new program side by side for one short crossfade"},
  flow("n12f", Y0, 2.1, [("Compile", "the edit becomes a new program"),
                         ("Match IDs", "nodes with the same semantic ID keep their state"),
                         ("Switch", "at a block boundary: 128 samples, 2.67 ms"),
                         ("Crossfade", "5 to 10 ms, only where the structure changed")], size=14)),
 ("n13", "TITLE_ONLY", {"t": "In the browser, the audio thread only renders; compiling runs in a worker"},
  flow("n13f", Y0, 2.2, [("Compile worker", "own WASM instance: compiles and packs four buffers"),
                         ("Main thread", "loads samples, forwards the packed buffers"),
                         ("Audio worklet", "process() every 2.67 ms: renders, loads buffers, nothing else")],
       hi=2)
  + [text("n13t", 0.5, Y0 + 2.45, 9.0, 0.8,
          "One blocking message handler on the worklet starves the output for its whole duration.",
          16, MUTED)]),
 # ---------------------------------------------------------------- batch 2: harness
 ("n14", "SECTION_HEADER", {"t": "Harness: can you trust vibe-coded real-time C++?"}, []),
 ("n15", "BIG_NUMBER", {"t": "48 GB",
   "b": "One akkado --check grew to 48 GB RSS (190 GB virtual). The machine slowing down found it; "
        "the unit suite was green."}, []),
 ("n16", "TITLE_ONLY", {"t": "Four failure types pass every assertion, so each gets its own test leg"},
  [table("n16t", Y0, [["Failure", "Leg", "Fails when"],
     ["Unbounded memory growth", "1  Explosion guard", "a corpus compile exceeds 1 GB RSS or its timeout"],
     ["Leaks, memory errors", "2  Sanitizers", "ASan, LSan or UBSan reports"],
     ["Allocation in the audio path", "3  Zero-alloc trap", "any new or malloc during block processing"],
     ["Drift across edits", "4  Drift fuzz", "peak RSS or its slope exceeds budget after thousands of hot-swaps"]],
     [2.6, 2.1, 4.3], ["START", "START", "START"], 14)]),
 ("n17", "TITLE_ONLY", {"t": "Leg 1: every corpus compile runs under a 1 GB ceiling, 48× below the incident"},
  [text("n17t", 0.5, Y0, 9.0, 3.2,
        "akkado --check runs over a fixture corpus with an RSS ceiling and a wall-clock timeout\n"
        "setrlimit inside akkado and nkido is the hard backstop (NKIDO_RLIMIT_MB)\n"
        "All budgets live in one file and are never raised without a stated reason",
        18, TXT, space_below=12)]),
 ("n18", "TITLE_ONLY", {"t": "Leg 2: the sanitizers found two real bugs on the first day"},
  flow("n18f", Y0, 2.2, [("UBSan", "memcpy from a null pointer when a program compiles to zero instructions"),
                         ("ASan", "heap-use-after-free in the analyzer: a reference to an optional "
                                  "that outlived its block")], size=16)),
 ("n19", "TITLE_ONLY", {"t": "Leg 3: a single allocation in the audio path fails the test"},
  [text("n19t", 0.5, Y0, 9.0, 3.0,
        "operator new and malloc are trapped while the VM processes a block\n"
        "Stateful opcodes take memory from pools sized before playback starts",
        18, TXT, space_below=12)]),
 ("n20", "TITLE_ONLY", {"t": "Leg 4: the drift fuzz found two crashes that only appear after many edits"},
  [text("n20x", 0.5, Y0, 9.0, 0.6,
        "Mutate seed programs, compile, hot-swap into one persistent VM, thousands of times.", 16, MUTED)]
  + flow("n20f", Y0 + 0.7, 2.3,
         [("Arena never reclaimed", "after about 150 structurally different FX programs the 32 MB arena "
           "was full; reverb got a null comb buffer and crashed. Fixed with a size-classed free list."),
          ("Pattern blocks leaked", "3,590 failed allocations at 100k iterations. After the fix: 0, "
           "with an arena peak of about 8.6 MB.")], size=14)),
 ("n21", "MAIN_POINT", {"t": "AI does not lower the bar for rigor. It moves the bottleneck from "
   "writing code to trusting it."}, []),
 # ---------------------------------------------------------------- batch 3: research loop, close
 ("n22", "SECTION_HEADER", {"t": "Research loop: an agent optimises the engine, the harness decides"}, []),
 ("n23", "TITLE_ONLY", {"t": "A change is kept only if it is faster and passes every gate"},
  flow("n23f", Y0, 2.6, [("Propose", "agent edits one opcode; 30 min; one check command"),
                         ("Build", "Release, AVX2"),
                         ("Gates", "null test at -100 dBFS on fixed and hidden random inputs; "
                                   "unit suites; DSP experiments"),
                         ("Benchmark", "pinned core, 5 rounds against the last accepted state"),
                         ("Decide", "commit if faster and green, otherwise revert")],
       gap=0.2, size=14, hi=2)),
 ("n24", "TITLE_ONLY", {"t": "12 models ran against 5 DSP targets, 8 iterations per run"},
  [text("n24h1", 0.5, Y0, 5.2, 0.5, "Models", 18, AMBER, bold=True),
   text("n24a", 0.5, Y0 + 0.5, 5.2, 2.9,
        "Claude Code: opus, sonnet\nOpenAI Codex: gpt-6-astra, gpt-5.6-sol, gpt-reserve\n"
        "OpenRouter: glm-5.3, deepseek-v4-pro, qwen3.8-27b and three cheap or free models\n"
        "Local: qwen3:14b via Ollama", 16, TXT, space_below=8),
   text("n24h2", 6.0, Y0, 3.5, 0.5, "Targets", 18, AMBER, bold=True),
   text("n24b", 6.0, Y0 + 0.5, 3.5, 2.9,
        "arith: add, sub, mul\ndistort: tanh, soft clip\nformant filter\nstate-variable filter\nfreeverb",
        16, TXT, space_below=8),
   foot("n24f", "311 completed iterations. OpenRouter runs are partial: the credits ran out mid-sweep.")]),
 ("n25", "TITLE_ONLY", {"t": "Claude models led on the hard DSP targets; arithmetic stopped near 1.4× for all"},
  [table("n25t", Y0 - 0.05, [["Model", "arith", "distort", "formant", "svf", "freeverb"],
     ["opus", "1.41×", "15.6×", "8.13×", "3.48×", "6.76×"],
     ["sonnet", "1.36×", "14.5×", "5.96×", "8.73×", "7.25×"],
     ["gpt-5.6-sol", "1.42×", "10.2×", "1.45×", "2.62×", "3.10×"],
     ["gpt-6-astra", "1.32×", "5.31×", "none", "1.44×", "4.12×"],
     ["gpt-reserve", "1.33×", "7.20×", "1.22×", "1.07×", "1.49×"],
     ["deepseek-v4-pro", "1.27×", "12.6×", "1.32×*", "", ""]],
     [2.4, 1.32, 1.32, 1.32, 1.32, 1.32], ["START"] + ["END"] * 5, 14),
   foot("n25f", "Best accepted speedup vs the original scalar code (geomean over opcodes). *3 of 8 iterations.")]),
 ("n26", "TITLE_ONLY", {"t": "Opus made tanh 31× faster in eight steps of 2 to 7 minutes each"},
  [table("n26t", Y0 - 0.05, [["Step", "Change", "ns per block", "vs original"],
     ["start", "scalar std::tanh", "1054", "1.0×"],
     ["1", "AVX2 kernel with an exp-based tanh approximation", "100", "10.5×"],
     ["2", "fewer vector operations in the approximation", "54", "18.7×"],
     ["3", "tanh as 1 - 2/(1 + e^2y)", "48", "21.5×"],
     ["4", "skip the dry/wet mix at default settings", "41", "24.7×"],
     ["5 to 8", "2^(j/8) lookup table, fewer FP-port operations", "34", "30.7×"]],
     [1.0, 5.0, 1.5, 1.5], ["START", "START", "END", "END"], 14),
   foot("n26f", "op_distort_tanh, one 8-iteration run with every iteration accepted; about 30 min, "
        "$4.69 of API-equivalent usage.")]),
 ("n27", "TITLE_ONLY", {"t": "The agent's own check passed three times; hidden random inputs rejected all three"},
  [table("n27t", Y0 - 0.05, [["Iteration", "Idea", "Agent's check", "Hidden inputs"],
     ["5", "polynomial instead of std::tan for small angles", "pass, 1.07×", "-96.6 dBFS"],
     ["6", "narrow tangent series, less Q-state traffic", "pass, 1.06×", "-97.5 dBFS"],
     ["7", "x⁹ approximation below 0.08 rad", "pass, 1.15×", "-95.1 dBFS"]],
     [1.2, 4.3, 1.8, 1.7], ["START", "START", "END", "END"], 14),
   text("n27x", 0.5, 3.45, 9.0, 1.4,
        "gpt-5.6-sol on the formant filter. The limit is -100 dBFS. The final verify draws a new random "
        "seed every time, so the agent cannot tune against the hidden inputs.", 16, TXT)]),
 ("n28", "TITLE_ONLY", {"t": "36% of 311 attempts were accepted; 9 faster patches changed the audio"},
  bars("n28b", Y0 - 0.05, [("accepted", 111), ("too slow", 96), ("empty patch", 42),
                           ("build failed", 30), ("timeout or backend", 23), ("output changed", 9)], hi=5)),
 ("n29", "MAIN_POINT", {"t": "The harness is what makes autonomy safe."}, []),
 ("n30", "TITLE", {"t": "Thank you",
   "b": "nkido.cc  ·  github.com/mlaass/nkido\nQuestions: the session's Discord channel"}, []),
]
BATCHES = {1: ("n01", "n07"), 2: ("n08", "n13"), 3: ("n14", "n21"), 4: ("n22", "n25"), 5: ("n26", "n30")}


def title_style(sid, layout):
    """TITLE_ONLY and SECTION_HEADER titles default to white here; the deck's titles are amber."""
    st = {"TITLE_ONLY": {}, "SECTION_HEADER": {"fontSize": {"magnitude": 32, "unit": "PT"}}}.get(layout)
    if st is None:
        return []
    st = {**st, "foregroundColor": {"opaqueColor": {"rgbColor": {"red": 0.945, "green": 0.761, "blue": 0.196}}}}
    return [{"updateTextStyle": {"objectId": f"{sid}_t", "textRange": {"type": "ALL"}, "style": st,
                                 "fields": ",".join(st)}}]


def main():
    rev, batch = sys.argv[1], int(sys.argv[2])
    lo, hi = BATCHES[batch]
    reqs, spec = [], {"slides": []}
    for sid, layout, ph, els in SLIDES:
        if not lo <= sid <= hi:
            continue
        maps = [{"layoutPlaceholder": {"type": t, "index": i}, "objectId": f"{sid}_{r}"}
                for t, i, r in PH[layout]]
        reqs.append({"createSlide": {"objectId": f"{sid}_s", "slideLayoutReference":
                     {"layoutId": L[layout]}, "placeholderIdMappings": maps}})
        for r, s in ph.items():
            reqs.append({"insertText": {"objectId": f"{sid}_{r}", "text": s}})
        reqs += title_style(sid, layout)
        if els:
            els = [{**e, "id": f"e_{e['id']}"} for e in els]  # API ids need >= 5 chars
            spec["slides"].append({"id": f"{sid}_s", "existing": True, "elements": els})
    if spec["slides"]:
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, dir=".") as f:
            json.dump(spec, f)
        r = subprocess.run([sys.executable, HELPER, "build", f.name, "--revision", rev],
                           capture_output=True, text=True)
        sys.stderr.write(r.stderr)
        reqs += json.loads(r.stdout)["requests"]
    def rnd(o):  # 16-digit colour floats -> 3 digits; keeps payloads small
        if isinstance(o, float): return round(o, 3)
        if isinstance(o, dict): return {k: rnd(v) for k, v in o.items()}
        if isinstance(o, list): return [rnd(v) for v in o]
        return o
    reqs = [r for r in rnd(reqs) if not (  # START is the default alignment: skip
        "updateParagraphStyle" in r and r["updateParagraphStyle"].get("style") == {"alignment": "START"})]
    amber = {"opaqueColor": {"rgbColor": {"red": 0.945, "green": 0.761, "blue": 0.196}}}
    if lo <= "n25" <= hi:  # best result per column in the results table
        for row, col in [(3, 1), (1, 2), (1, 3), (2, 4), (2, 5)]:
            reqs.append({"updateTextStyle": {"objectId": "e_n25t", "cellLocation": {"rowIndex": row,
                         "columnIndex": col}, "textRange": {"type": "ALL"},
                         "style": {"bold": True, "foregroundColor": amber}, "fields": "bold,foregroundColor"}})
    if lo <= "n26" <= hi:  # the final row is the headline number
        for col in range(4):
            reqs.append({"updateTextStyle": {"objectId": "e_n26t", "cellLocation": {"rowIndex": 6,
                         "columnIndex": col}, "textRange": {"type": "ALL"},
                         "style": {"bold": True, "foregroundColor": amber}, "fields": "bold,foregroundColor"}})
    print(json.dumps({"requests": reqs, "writeControl": {"requiredRevisionId": rev}},
                     ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main()

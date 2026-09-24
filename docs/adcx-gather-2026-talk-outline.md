# ADCx Gather 2026 — Talk Outline

**Compiling Music** — NKIDO: A Live-Coding Audio Language with a Zero-Allocation C++20 Bytecode VM

- **Slot:** Fri 16 Oct 2026, 19:30–19:50 UTC (Sat 06:30 Sydney AEDT / Fri 21:30 Berlin)
- **Format:** 18 min, **pre-recorded**; be live in the session's Discord channel during playback
- **Abstract:** as published on the schedule (same as `adc-bristol-2026-proposal.md`). The outline must deliver
  everything it promises: demo, pipes vs SC/Tidal, mini-notation, coerce-first types, overloads,
  hot-swap, browser audio thread, 4-leg harness, autonomous research loop.
- **Shape:** harness-centred. Demo flow reused from ADC Japan (updated syntax), new slides.

## Deadlines

| Date | What |
|---|---|
| Mon 21 Sep | ~1 min test recording (same setup; include a demo snippet with audio) |
| ~Mon 28 Sep | Research loop working + one real run captured (see §6) |
| Fri 2 Oct | Final video uploaded |
| Fri 16 Oct | Playback; online in Discord |

## Run sheet

| Time | Section | Length |
|---|---|---|
| 0:00 | Title card pre-roll (6 s) + hook | 0:30 |
| 0:30 | 1. Demo | 3:00 |
| 3:30 | 2. Language | 3:00 |
| 6:30 | 3. Runtime | 3:00 |
| 9:30 | 4. Harness — the centrepiece | 6:00 |
| 15:30 | 5. Research loop + close | 2:30 |

---

## 0. Hook (0:30)

- One sentence: "A live-coding language and a real-time C++ engine, built largely by AI. This talk is
  about why I trust it."
- Numbers slide: ~72k lines C++ engine + compiler, **~65k lines C++ tests**, ~29k TS/Svelte IDE,
  155 opcodes, 87 per-opcode DSP verification scripts. MIT.
  (Test code ≈ engine code is the setup for §4.)

## 1. Demo (3:00) — reuse the ADC Japan flow, edited

Screen-recorded in the browser IDE, jump-cut for pace (pre-record lets us compress 8 min of
typing into 3). No slides.

1. Empty editor → one oscillator → `out`
2. Pattern: `n"…"` notes, then `[…]` subdivision and a Euclidean rhythm
3. Chords: `c"Am F C G"` / `n"Am F C G"` → `e.freqs` into parallel voices
   (Japan used `C4'` — **removed syntax, rewrite**; `%` → `@`)
4. Filter sweep + delay/reverb chain; `param()` sliders appear in the UI
5. Edit while playing: phases and filter state survive the swap (the §3 payoff)

Starting material: `web/static/patches/welcome/` (`09-euclid-bass`, `10-acid-303`).

## 2. Language (3:00)

- **Pipes & holes (0:45)** — one slide, same patch in SuperCollider / Tidal / Akkado.
  `|>` reads left to right, `@` is "the signal so far", `as` names a stage.
- **Mini-notation (0:45)** — a real parser inside the string: alternation per cycle, `[…]`
  subdivision, Euclid, polyrhythm, chord symbols → events with `.freq/.vel/.trig/.gate`.
- **Types that coerce, not reject (0:45)** — a hard type error mid-set is the wrong default.
  Coerce where defensible, warn (W-class) instead of failing. Concrete example: a void argument
  becomes silence instead of an error (`bff483f`). Where it bites back: silent wrong-ish output.
- **Overload resolution (0:45)** — one name, many forms; the compiler picks by argument shape,
  for builtins and user functions alike.

## 3. Runtime (3:00)

- **Instructions + state pool (1:00)** — fixed-width instructions, buffers by index, every stateful
  thing in one pool keyed by a stable semantic ID (hash of its source path).
- **Hot-swap (1:00)** — old and new program side by side, switch at a block boundary, matching IDs
  keep their state, short crossfade for structural changes. Callback to the demo.
- **Browser audio thread (1:00)** — `process()` fires every 2.67 ms; any blocking message starves
  output. So the worklet only renders and loads packed buffers; compile runs in a separate worker
  with its own WASM instance.

## 4. Harness (6:00) — "can you trust vibe-coded real-time C++?"

- **The incident (0:45)** — one `akkado --check` ballooned to **48 GB RSS / 190 GB VSZ**. Found by
  the machine slowing down, not by a test. The unit suite was green.
- **Why green hides it (0:30)** — unbounded growth, leaks, audio-path allocations, slow drift:
  none of them fail an assertion.
- **Leg 1 — Explosion guard (0:45)** — compile a corpus under an RSS ceiling + timeout;
  `setrlimit` backstop in the CLIs. Budget 1 GB = 48× under the incident. All budgets in one file,
  never silently raised.
- **Leg 2 — Sanitizers (0:45)** — ASan/LSan/UBSan preset. Real catches on day one:
  UBSan `memcpy` from null on a zero-instruction compile; ASan heap-use-after-free in the analyzer
  (a reference to an `optional` dangling past its block).
- **Leg 3 — Zero-alloc trap (0:45)** — `operator new`/`malloc` trap armed around block processing;
  one allocation = hard fail.
- **Leg 4 — Drift fuzz (1:45)** — mutate seed programs (token / structural / grammar-synth),
  compile, hot-swap into **one persistent VM**, thousands of times; assert peak RSS and slope.
  The two bugs it found are the "every few bars" story:
  1. Arena never reclaimed on hot-swap → after ~150 structurally different FX programs the 32 MB
     arena filled, reverb got a null comb buffer, crash. Fix: size-classed free list + null-buffer
     guard. The fuzz is now its regression test.
  2. At 100k iterations: pattern-sequence blocks leaked on every re-init → 3,590 failed allocations.
     After the fix: 0 exhaustions, arena peak ~8.6 MB.
- **Takeaway (0:30)** — AI doesn't lower the bar for rigor, it raises it: the bottleneck moves from
  writing code to trusting it. Pre-release gate runs every leg at depth.

## 5. Research loop + close (2:30) — **not built yet; target ~28 Sep**

> Proposal, not a decision — design to be settled before building.

- **Idea (0:45)** — with the harness as the safety net, point an agent at the engine's own DSP
  performance and let it iterate unattended.
- **Loop (sketch)** — pick a hot opcode → propose a change → build → gates: unit tests,
  zero-alloc trap, drift fuzz, that opcode's Python experiment (output compared against the
  baseline) → benchmark → keep only if faster **and** every gate is green, else revert.
- **Show (1:15)** — time-lapse of a real run; one accepted change with before/after numbers;
  one rejected change the harness caught.
- **Close (0:30)** — "The harness is what makes autonomy safe." Links: nkido.cc, GitHub, Discord
  channel for questions.

---

## Open items

- [ ] Design + build the research loop; decide what "DSP performance" is measured by
- [ ] Verify before claiming: was the 48 GB `akkado --check` repro itself fixed?
- [ ] Leg 3: find a real zero-alloc trap catch in history, or show an injected violation failing
- [ ] Rewrite the Japan demo script for current syntax (`c"…"`/`n"…"` chords, `@` holes,
      top-level = per-cycle alternation)
- [ ] SC / Tidal / Akkado side-by-side slide (same patch in all three)
- [ ] Refresh the numbers slide right before recording

## Production notes (from the ADCx speaker resources)

- **Export (test + final):** 1920×1080, MP4/MOV, H.264, 30 fps, ≥8 Mbps (16+ preferred),
  AAC or PCM, 48 kHz stereo
- Title card as a 6 s full-screen pre-roll; session background artwork from their Dropbox
  (layout reference first, then swap in the real background)
- Slides 16:9, ≥16 pt; enlarge the editor font for the demo
- Capture browser audio directly in OBS (not through the mic)
- They offer editing help if raw recordings are delivered instead

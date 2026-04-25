# Polyphony cycle-alignment bugs at "rational" BPMs

**Date:** 2026-04-25
**Area:** `cedar/src/vm/vm.cpp` — `execute_poly_block` event scheduling
**Symptom (user-reported):** Chord-stab patches sound wrong "every few bars" — sometimes a short voice gap, sometimes a chord coming out at ⅓ amplitude.
**Reproducer patch:**
```akkado
bpm = 110
stab = (freq, gate, vel) ->
    saw(freq) * ar(gate, 0.05, 0.4) * vel
    |> lp(@, 1200)
    |> @ + delay(@ , 0.3, 0)

chord("C Em Am F")
    |> poly(@, stab) * 0.33
    |> out(@)
```
**Trigger condition:** any BPM where `cycle_length_in_samples` is a rational multiple of `BLOCK_SIZE`. At `BPM=110, sr=48000, BLOCK_SIZE=128, cycle=4 beats`, `4·spb = 1,152,000/11`, so every 11 cycles aligns to exactly 9000 blocks — and within those 11 cycles, every chord (every beat) periodically lands on a block boundary too.

---

## TL;DR

Three independent bugs all hidden at this same "exact alignment" condition. None are visible in short renders because alignment events are rare; all are deterministic at long renders.

| # | Symptom | Root cause | Fix location |
|---|---|---|---|
| 1 | One block of "no firing voices" at cycle wrap (block 8999 → 9000) | `off_upper` epsilon hack fires gate-off in *previous* block, but the next-cycle gate-on detection uses strict `>` so it misses the new chord | `vm.cpp:355–384` (gate-off scheduling) |
| 2 | Voices stay alive for entire cycles, 4 voices firing instead of 3 (block 99204+) | `fmod(beat, cycle_length)` in `double` returns ~5.7e-14 instead of 0 at certain large sample counts; `evt_start >= cycle_pos` then fails for time=0 events | `vm.cpp:279–328` (cycle-pos snapping) |
| 3 | Aligned chord transitions where shared frequencies are retriggered come out at ⅓ amplitude | Gate buffer `[1,1,…,1]` has no 0→1 edge across the block boundary, so `ENV_AR` never sees a rising edge and never re-attacks | `vm.cpp:443–461` (gate buffer fill) |

All three are now covered by hard assertions in `experiments/test_op_poly.py` against a 300-second render of `chord-stab.akk`.

---

## Why these bugs hid

The poly opcode schedules events in the **beats** domain (`cycle_pos`, `evt_start`, `evt_end` are in beats), not samples. That's natural — events have float beat times — but it means floating-point comparisons between event times and the running cycle position can land on either side of an integer boundary depending on rounding. At "irrational" BPMs (e.g., 120, 140) the cycle never aligns exactly with a block, so `cycle_pos` always drifts cleanly past each beat boundary inside some block. At BPM 110 (and similar rationals at 48 kHz), the alignment is exact and exposes every off-by-epsilon comparison.

A 40-second render only saw alignment once and produced a subtle 1-block trace anomaly. Going to 300 seconds (per the [`docs/dsp-experiment-methodology.md`](../dsp-experiment-methodology.md) ≥300s rule for sequenced opcodes) produced a 1-second-long sustained 4-voice run that was trivially audible — and exposed the second bug. The third bug was an audio-amplitude anomaly invisible to the voice-allocation trace and required per-chord RMS analysis to surface.

**Process lesson:** for any opcode that schedules events from a beat-domain pattern, ≥300 seconds of simulated audio at a "rational" BPM (110, 165, 132, etc.) is the table-stakes test. Trace-only checks are not enough; audio-amplitude checks (per-event RMS, peak, spectrum) catch a separate class of bug.

---

## Bug 1 — single-block voice gap at cycle wrap

### Symptom
Voice trace shows exactly one block (block 8999 of a 40-second render at BPM 110) where all three active voices have `gate=0, releasing=true` and zero new voices are firing. The 16 other cycle-wraps in the render show the normal 3-firing-voices pattern.

### Root cause
At the cycle 10 → cycle 11 transition that lands at block 9000:
- Block 8999: `cycle_pos = 3.9951`, `block_end_pos = 4.0` exactly (because `9000 × 128 / spb = 44.0 beats`).
- For G's gate-off (`evt_end = 4.0`):
  - The `off_upper` epsilon hack at the old `vm.cpp:363–365` extended the upper bound by `1e-4f`, so `evt_end < off_upper` fired and G voices were released at sample 127 of block 8999.
- For C's gate-on (`evt_start = 0.0`, next cycle):
  - Case A (`evt_start >= cycle_pos`) fails: `0.0 >= 3.9951` is false.
  - Case B (`block_end_pos > cycle_length`) fails: `4.0 > 4.0` is false (strict `>`).
  - C is **not** allocated until block 9000 sample 0.

Result: end-of-block 8999 = G released, C not yet allocated.

### Fix
Replace the `off_upper` epsilon hack with a wrap branch that uses `evt_end >= cycle_length` and a `current_cycle > 0` guard. At exact alignment the gate-off now fires in the *new cycle's first block* at sample 0, alongside the new cycle's gate-ons — exactly matching the behavior at every non-aligned wrap.

```cpp
} else if (evt_end >= cycle_length && current_cycle > 0) {
    float wrapped_end = evt_end - cycle_length;
    if (wrapped_end >= cycle_pos && wrapped_end < block_end_pos) {
        gate_off_this_block = true;
        off_beat_offset = wrapped_end - cycle_pos;
        off_cycle = current_cycle - 1;
    } else if (block_end_pos > cycle_length) { /* ... */ }
}
```

The `current_cycle > 0` guard prevents the new branch from spuriously releasing a just-allocated voice in cycle 0 (where there is no previous cycle to release from).

### Test
`experiments/test_op_poly.py` asserts `firing < 3` is never observed across a 300-second render.

---

## Bug 2 — float precision at high sample counts

### Symptom
After Bug 1's fix, a 40-second render passed but a 300-second render showed 409 consecutive blocks (99204..99612, ≈1 second) with **four** firing voices instead of three. The extra voice was the previous chord's unique note, never released.

### Root cause
At block 99000 (cycle 121 boundary, sample 12,672,000), exact arithmetic gives `beat = 484.0`, `cycle_pos = 0.0`. But IEEE-754 `double` arithmetic rounds the chain `12672000 / (60.0/110.0 * 48000.0)` to `484.0 + ~1.1e-14`, so `fmod(beat, 4.0)` returns `5.68e-14` rather than `0.0`. After the cast to `float` the residue survives as `5.68e-14f` (representable as a normal float32).

Then for any event with `evt_start = 0.0`:
- Case A: `0.0 >= 5.68e-14` is **false**.
- Case B: `block_end_pos (≈0.005) > cycle_length (4.0)` is false.
- Gate-on never fires. C voices are never allocated for cycle 121.

For G's gate-off at the boundary, Bug 1's fix made the wrap branch require `wrapped_end >= cycle_pos`, but `0.0 >= 5.68e-14` is also false. So G voices are never released either. They keep firing through cycle 121 until the next chord transition stomps over them.

The residue magnitude depends on the specific sample count. Block 9000 (sample 1,152,000) happened to round to exactly 0 — that's why the 40-second render passed. Block 99000 didn't.

### Why "1e-9" is the right epsilon
- Sample resolution at 110 BPM / 48 kHz: `1 sample = 1/spb beats ≈ 3.8e-5 beats`. Any cycle-position error below ~3.8e-5 beats is sub-sample and cannot represent a real time difference.
- The empirically observed residue is ~5.7e-14, growing roughly linearly with sample count. After a 6-hour render at 48 kHz, the residue would still be well below 1e-9.
- 1e-9 is therefore comfortably above fp noise and ~5 orders of magnitude below sample resolution.

### Fix
Snap `cycle_pos` to 0 (and `block_end_pos` to `cycle_length`) within `1e-9` beats. Done in `double` before the cast to `float`:

```cpp
constexpr double CYCLE_BOUNDARY_EPSILON = 1e-9;
double cycle_pos_d_raw = std::fmod(beat_start_d, cycle_length_d);
if (cycle_pos_d_raw < CYCLE_BOUNDARY_EPSILON) cycle_pos_d_raw = 0.0;
const double cycle_pos_d = cycle_pos_d_raw;
// ...
double block_end_pos_d_raw = cycle_pos_d + block_beats_d;
if (std::abs(block_end_pos_d_raw - cycle_length_d) < CYCLE_BOUNDARY_EPSILON) {
    block_end_pos_d_raw = cycle_length_d;
}
```

The same snap is applied in the `CEDAR_FLOAT_ONLY` path with the epsilon cast to `float`.

### Test
The 300-second render's voice trace now shows `firing min=3 max=3 mean=3.00`. The poly test's hard `firing > 3` assertion would catch this regression.

---

## Bug 3 — envelope retrigger on aligned blocks

### Symptom
After Bugs 1 & 2 were fixed, the voice trace was clean — 3 firing voices throughout, all at the right frequencies. But the audio still had a problem: certain chords sounded **at ⅓ amplitude on certain cycles**. With `chord("C Em Am F")` at BPM 110, the affected chords were exactly:

- `Em` at cycle 8, 19, 30 …  *(K mod 11 = 8)*
- `F` at cycle 2, 13, 24 …   *(K mod 11 = 2)*

Not `C` (K mod 11 = 0) and not `Am` (K mod 11 = 5).

### Why those chords specifically
Within a 4-beat cycle, beat *B* of cycle *K* lands on a block boundary iff `(4K + B) ≡ 0 (mod 11)`. With `chord("C Em Am F")`:

| Chord | B | Aligned at K mod 11 = | Shares notes with previous chord? |
|-------|---|----|--------------------------------|
| C  | 0 | 0  | F → C: **no shared notes** |
| Em | 1 | 8  | C → Em: **E4, G4 shared** |
| Am | 2 | 5  | Em → Am: **no shared notes** |
| F  | 3 | 2  | Am → F: **A4, C5 shared** |

The only chords with the bug are the ones whose transition has shared frequencies. That's the smoking gun.

### Root cause
`PolyAllocState::allocate_voice` re-uses the existing voice slot when a frequency match is found in the current voices, to preserve oscillator phase. When the new chord starts on an aligned block (gate-on at sample 0), the gate buffer is filled by `vm.cpp`:

```cpp
if (voice.pending_gate_on < BLOCK_SIZE) {
    std::fill_n(gate_buf, voice.pending_gate_on, 0.0f);  // 0 samples filled
    std::fill_n(gate_buf + voice.pending_gate_on,
                BLOCK_SIZE - voice.pending_gate_on, 1.0f);
    trig_buf[voice.pending_gate_on] = 1.0f;
}
```

For `pending_gate_on = 0` this produces `gate_buf = [1,1,…,1]` — no 0→1 transition inside the block.

`ENV_AR` retriggers on a rising edge of its trigger input (which the patch wires to `gate`):
```cpp
bool trigger_on = (current_trigger > 0.0f && state.prev_gate <= 0.0f);
state.prev_gate = current_trigger;
```

For a **fresh** voice allocation, the slot was inactive last block, so AR's `prev_gate` stayed 0 from its last activity (during the previous activation's release period when `voice.gate = 0`). Sample 0: `prev=0, curr=1` → trigger ✓.

For a **retriggered** (shared-frequency) voice, the slot was firing the previous chord last block with `voice.gate = 1` and `gate_buf` filled with 1s, so AR's stored `prev_gate = 1`. Sample 0 of the new block: `prev=1, curr=1` → no trigger. AR never re-attacks; it continues from whatever state it was already in (typically the late-release tail, so `level ≈ 0.05`). The audio of that voice comes out at envelope-tail amplitude, drowned by the one fresh voice in the chord.

For non-aligned chord starts (`pending_gate_on > 0`), the gate buffer naturally has a `[0,…,0,1,…,1]` shape: there's a falling edge at sample 0 then a rising edge at `pending_gate_on`, which AR catches. **This is why the bug only appeared at exact alignment.**

### Why amplitude is ⅓, not 0
Of the 3 voices in each chord, one was a fresh allocation (the chord's unique note — `F4` in F, `B4` in Em) and retriggered correctly. The other two had shared frequencies and never re-attacked. So the amplitude was 1 of 3 voices ≈ ⅓.

### Fix
Force `gate_buf[0] = 0` whenever `pending_gate_on == 0`. The new gate buffer is `[0, 1, 1, …, 1]`, which puts the rising edge at sample 1. AR retriggers normally for both fresh and retriggered voices; the cost is one sample (~21 µs) of envelope-attack delay at aligned chord starts.

```cpp
if (voice.pending_gate_on < BLOCK_SIZE) {
    std::fill_n(gate_buf, voice.pending_gate_on, 0.0f);
    std::fill_n(gate_buf + voice.pending_gate_on,
                BLOCK_SIZE - voice.pending_gate_on, 1.0f);
    trig_buf[voice.pending_gate_on] = 1.0f;
    if (voice.pending_gate_on == 0) {
        gate_buf[0] = 0.0f;
    }
}
```

For fresh allocations this is a no-op semantically (AR's `prev_gate` was already 0; a new 0 at sample 0 doesn't change anything). For retriggers, it's the missing edge.

### Test
Added per-chord RMS stability check in `experiments/test_op_poly.py::test_chord_stab_audio_quality`. For each chord (C/Em/Am/G), it asserts `min_cycle_rms / mean_cycle_rms >= 0.7`. Before the fix, the affected chords had ratio ≈ 0.55; after, all chords have ratio > 0.96.

---

## Patterns and where to look next

These three bugs share a structure that is worth keeping in mind for the rest of the engine.

### "Rational BPM" alignment is a real edge case
Any code that takes `(global_sample_counter, bpm, sample_rate, block_size)` and computes "where am I in the bar/beat/cycle" can collapse onto an integer block boundary at certain BPMs. Test at:
- BPM 110 at 48 kHz: every 11 cycles align (smallest whole-cycle alignment within a sane BPM range)
- BPM 120 at 48 kHz: every cycle aligns (`4·spb·k % 128 = 0` for all `k` because `spb = 24000`, so every block boundary IS a beat boundary)
- BPM 132, 150, 165 also have short alignment periods

**Rule of thumb:** if a feature interacts with beat scheduling, render `chord("C Em Am F") |> poly(@, stab)` at BPM 110 for ≥300 seconds and verify per-chord RMS, voice trace, and audio quality. Do *not* trust BPM 120 to expose alignment bugs — it aligns *every* cycle, which makes the bug uniform and easy to mistake for "intended behavior."

### Beat-domain `>=` vs sample-domain truth
Comparisons like `evt_start >= cycle_pos` are dangerous when both sides come from float arithmetic on large sample counts. Whenever you write a `>=` or `<` between a beat-domain event time and a fmod'd cycle position, ask:
- What residue does the fmod produce at sample counts ~10^7 / ~10^8?
- Is the residue larger or smaller than 1-sample resolution?
- If smaller, snap.

Snapping at the beat-domain comparison is cheap and makes the code immune to rounding drift. The 1e-9-beat epsilon used in vm.cpp is well-justified: ~5 orders of magnitude below sample resolution, ~5 orders of magnitude above accumulated fp noise even at hour-long renders.

### "DSP state per voice slot" implies "the slot's history bleeds into reuse"
Every voice slot has its own AR state, oscillator phase, filter state, delay buffer (via `state_id_xor`). When the slot is reused for a new note, that history affects the new note. For most cases this is the design intent (preserves phase, avoids clicks). But it means **any opcode that detects rising/falling edges of gate is at the mercy of what `prev_gate` was at the slot's last block of activity.**

Things to audit with this lens:
- Other envelope opcodes: ADSR, AR, anything that retriggers on edges. Verify they all see a rising edge on retrigger at aligned blocks. (`ENV_ADSR` likely has the same bug — check it.)
- Trigger inputs to oscillators (`saw`/`sqr`/`tri` all have an optional `trig` input that resets phase via `check_phase_reset`, which is also rising-edge-detecting). The fix in `vm.cpp` writes `trig_buf[pending_gate_on] = 1.0f`; for retriggered voices at sample 0 this depends on the trig buffer being zero before this assignment, which it is (it gets filled with 0 at `vm.cpp:440`). So oscillator phase reset is fine. But verify any opcode that reads gate or trig and expects a leading 0.
- Sample-trigger opcodes (Karplus-Strong, sample player triggers): same logic.

### When voice trace looks fine, listen to the audio
Bug 3 was invisible to the voice trace — `firing=3` throughout the affected chord. Only per-chord audio analysis (RMS / spectrum) caught it. The poly debug trace shows allocation/release events but not envelope-state correctness. For any future poly-related issue, two things must agree: the trace and the audio. If the trace looks clean and the audio sounds wrong, the bug is downstream of voice allocation — in body opcode state or in the gate/trig buffer fill.

### Extend test coverage proactively
The existing test suite for `[poly]` had unit tests with single-cycle, BPM-120 setups and 5-block renders. It missed all three bugs because:
1. Single cycle never hit alignment effects beyond cycle 0.
2. BPM 120 has uniform alignment, so per-cycle variation is zero.
3. 5 blocks is way too short for fp drift to grow.

The Python `test_op_poly.py` runs through `nkido-cli render` over 300 seconds at BPM 110 and now has three hard assertions:
- `firing > 3` is never observed (catches Bug 2 / voice leak)
- `firing < 3` is never observed (catches Bug 1 / voice gap)
- per-chord RMS ratio `min/mean >= 0.7` (catches Bug 3 / envelope retrigger failure)

When adding new poly-adjacent features (envelope variants, voice allocation modes, body-opcode changes), keep this test green — and ideally add a similar test for the new feature with a chord progression that creates shared-frequency retriggers.

---

## Files changed

- `cedar/src/vm/vm.cpp:279–328` — Bug 2 fix (cycle-pos snapping)
- `cedar/src/vm/vm.cpp:355–384` — Bug 1 fix (gate-off wrap-detection rewrite)
- `cedar/src/vm/vm.cpp:443–461` — Bug 3 fix (gate buffer edge forcing)
- `experiments/test_op_poly.py` — added `firing < 3` and per-chord RMS assertions; render duration extended to 300 s

## Verification

- `build/cedar/tests/cedar_tests`: 330,233 / 330,233 assertions, 140 / 140 cases pass
- `build/akkado/tests/akkado_tests`: 85,066 / 85,066 assertions, 472 / 472 cases pass
- `experiments/test_op_poly.py` at 300 s, BPM 110: all assertions pass; per-chord RMS ratios all > 0.96

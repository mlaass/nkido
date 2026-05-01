"""
SOUNDFONT_VOICE Retrigger Behavior Tests
=========================================

Validates the fixes shipped for `prd-soundfont-playback-fixes.md`:

  1. Sustained-gate sequential notes retrigger via the trigger pulse input
     (PRD Goal 1, the bug "only first note plays").
  2. Same-note repeats fast-release the old voice and start a new one
     (PRD Goal 1, re-articulation behavior).
  3. Note-change fallback works when the trigger is not wired (PRD §3.1.2).
  4. Gate-edge detection still triggers when notes have rest gaps.
  5. Velocity scales output amplitude.
  6. Vibrato-class pitch modulation does not cause spurious retriggers
     (PRD §7.1 Edge Case).
  7. Gate-off concurrent with a freq change releases voices and does not
     start a new voice (PRD §7.4 Edge Case).

The test drives synthetic buffers for gate / freq / vel / preset / trigger
directly into the SOUNDFONT_VOICE opcode — no Akkado compiler involvement.
A real SF3 fixture (TimGM6mb.sf3 from the web app) is required for the
`SOUNDFONT_VOICE` to find sample data; tests skip gracefully if the fixture
is missing.

If a test fails, the output WAV files in `experiments/output/op_soundfont/`
are the first place to listen — your ears will catch nuances RMS won't.
"""

from __future__ import annotations

import os
import sys

import numpy as np
from scipy.io import wavfile

import cedar_core as cedar
from cedar_testing import output_dir


OUT = output_dir("op_soundfont")
SR = 48000
BS = cedar.BLOCK_SIZE  # 128

# Buffer layout (these don't change between scenarios — keep them simple)
BUF_GATE = 0
BUF_FREQ = 1
BUF_VEL = 2
BUF_PRESET = 3
BUF_TRIG = 4
BUF_OUT = 10


def find_fixture() -> str | None:
    """Locate a real SF3/SF2 file. Walk up from CWD looking for the web-app
    soundfonts. Tests skip if no fixture is present."""
    here = os.path.dirname(os.path.abspath(__file__))
    cur = here
    for _ in range(8):
        for name in ("TimGM6mb.sf3", "FluidR3Mono_GM.sf3", "MuseScore_General.sf3"):
            cand = os.path.join(cur, "web", "static", "soundfonts", name)
            if os.path.exists(cand):
                return cand
        parent = os.path.dirname(cur)
        if parent == cur:
            break
        cur = parent
    return None


def freq_for_midi(n: int) -> float:
    return 440.0 * (2.0 ** ((n - 69) / 12.0))


def midi_to_name(n: int) -> str:
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    return f"{names[n % 12]}{n // 12 - 1}"


def save_wav(name: str, audio: np.ndarray, sr: int = SR) -> str:
    """Write float audio as 16-bit PCM in the test output dir."""
    path = os.path.join(OUT, name)
    clipped = np.clip(audio, -1.0, 1.0)
    pcm = (clipped * 32767).astype(np.int16)
    wavfile.write(path, sr, pcm)
    return path


def make_sf_program(sf_id: int, with_trigger: bool, state_id: int = 0xC0FFEE) -> list:
    """Build a tiny program that runs SOUNDFONT_VOICE and routes its output."""
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.SOUNDFONT_VOICE,
        BUF_OUT,
        BUF_GATE,
        BUF_FREQ,
        BUF_VEL,
        BUF_PRESET,
        BUF_TRIG if with_trigger else 0xFFFF,
        state_id,
    )
    inst.rate = sf_id  # SoundFont ID lives in the rate field
    output_inst = cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, BUF_OUT)
    return [inst, output_inst]


def run_blocks(vm, gate, freq, vel, preset, trig, num_blocks):
    """Drive the input buffers per-block, return concatenated mono output.

    All input arrays must be length num_blocks * BS. `trig` may be None,
    in which case the BUF_TRIG buffer is left unused (matching the
    inputs[4] = 0xFFFF case in the program)."""
    out_blocks = []
    for b in range(num_blocks):
        s, e = b * BS, (b + 1) * BS
        vm.set_buffer(BUF_GATE, gate[s:e].astype(np.float32))
        vm.set_buffer(BUF_FREQ, freq[s:e].astype(np.float32))
        vm.set_buffer(BUF_VEL, vel[s:e].astype(np.float32))
        vm.set_buffer(BUF_PRESET, preset[s:e].astype(np.float32))
        if trig is not None:
            vm.set_buffer(BUF_TRIG, trig[s:e].astype(np.float32))
        left, _right = vm.process()
        out_blocks.append(left.copy())
    return np.concatenate(out_blocks)


def rms(x: np.ndarray) -> float:
    if len(x) == 0:
        return 0.0
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))


def make_pulse(num_samples: int, sample_indices) -> np.ndarray:
    """1-sample-wide unit pulses at the given sample indices."""
    arr = np.zeros(num_samples, dtype=np.float32)
    for i in sample_indices:
        if 0 <= i < num_samples:
            arr[i] = 1.0
    return arr


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_single_note(vm, sf_id) -> bool:
    """Test 1: gate-edge alone produces a single sustained note that
    decays after gate-off. Validates the baseline path still works."""
    print("\nTest 1: Single sustained note (gate edge)")

    duration_s = 1.5
    n = int(duration_s * SR)
    n = (n // BS) * BS
    gate = np.zeros(n, dtype=np.float32)
    gate_on = int(0.05 * SR)
    gate_off = int(0.8 * SR)
    gate[gate_on:gate_off] = 1.0
    freq = np.full(n, freq_for_midi(60), dtype=np.float32)  # C4
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)

    program = make_sf_program(sf_id, with_trigger=False)
    vm.load_program(program)

    audio = run_blocks(vm, gate, freq, vel, preset, None, n // BS)

    pre = rms(audio[:gate_on])
    during = rms(audio[gate_on + 100 : gate_off])  # skip attack
    after = rms(audio[gate_off + int(0.1 * SR):])  # well into release

    save_wav("test1_single_note.wav", audio)
    print(f"  pre-gate RMS={pre:.5f}, during-gate RMS={during:.5f}, post-release RMS={after:.5f}")

    ok = pre < 1e-4 and during > 1e-3 and after < during
    print(f"  {'PASS' if ok else 'FAIL'}: single sustained note plays and releases")
    return ok


def test_sequential_notes_with_trigger(vm, sf_id, long_trace: bool = False) -> bool:
    """Test 2: THE critical test for the bug. Gate stays high; trigger pulses
    fire at each note boundary. All three notes must produce audio."""
    print("\nTest 2: Sequential C4 -> E4 -> G4 with trigger pulses (sustained gate)")

    duration_s = 1.5
    n = (int(duration_s * SR) // BS) * BS
    third = n // 3

    gate = np.ones(n, dtype=np.float32)
    freq = np.empty(n, dtype=np.float32)
    freq[:third] = freq_for_midi(60)        # C4
    freq[third:2 * third] = freq_for_midi(64)  # E4
    freq[2 * third:] = freq_for_midi(67)    # G4
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)
    trig = make_pulse(n, [0, third, 2 * third])

    program = make_sf_program(sf_id, with_trigger=True)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, trig, n // BS)

    # Skip the attack region of each note when measuring (envelope ramps).
    skip = int(0.05 * SR)
    rms_c = rms(audio[skip:third])
    rms_e = rms(audio[third + skip:2 * third])
    rms_g = rms(audio[2 * third + skip:])

    save_wav("test2_sequential_with_trigger.wav", audio)
    print(f"  RMS: C4={rms_c:.5f}  E4={rms_e:.5f}  G4={rms_g:.5f}")

    # Pre-fix bug: only C4 had audio. Post-fix: all three should be audible.
    threshold = 1e-3
    ok = rms_c > threshold and rms_e > threshold and rms_g > threshold
    print(f"  {'PASS' if ok else 'FAIL'}: all three notes audible")

    if long_trace:
        # Soak run: simulate 300+ seconds of the same pattern to flush out
        # any cumulative drift (per CLAUDE.md guidance for pattern-style tests).
        loops = int(np.ceil(300.0 / duration_s))
        program = make_sf_program(sf_id, with_trigger=True, state_id=0xC0FFEE)
        vm.load_program(program)
        # Reuse the same per-block buffers — VM holds state across blocks.
        soak_failures = 0
        for _ in range(loops):
            audio_chunk = run_blocks(vm, gate, freq, vel, preset, trig, n // BS)
            r1 = rms(audio_chunk[skip:third])
            r2 = rms(audio_chunk[third + skip:2 * third])
            r3 = rms(audio_chunk[2 * third + skip:])
            if min(r1, r2, r3) <= threshold:
                soak_failures += 1
        total_seconds = loops * duration_s
        print(f"  Soak: {loops} loops ({total_seconds:.0f}s), {soak_failures} failed iterations")
        ok = ok and soak_failures == 0

    return ok


def test_same_note_repeat_with_trigger(vm, sf_id) -> bool:
    """Test 3: same MIDI note retriggered three times with a trigger pulse.
    Each repeat fires the same-note fast-release path, so we expect three
    distinct envelope onsets visible in the waveform."""
    print("\nTest 3: Same-note repeat (C4 x3) with trigger pulses")

    duration_s = 1.5
    n = (int(duration_s * SR) // BS) * BS
    third = n // 3

    gate = np.ones(n, dtype=np.float32)
    freq = np.full(n, freq_for_midi(60), dtype=np.float32)  # constant C4
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)
    trig = make_pulse(n, [0, third, 2 * third])

    program = make_sf_program(sf_id, with_trigger=True)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, trig, n // BS)

    save_wav("test3_same_note_retrigger.wav", audio)

    # A re-articulation produces a transient near each pulse. Detect them by
    # looking for short-window peaks above a threshold near each pulse index.
    win = int(0.02 * SR)  # 20 ms after each pulse
    onsets_with_signal = 0
    for idx in (0, third, 2 * third):
        seg = audio[idx:idx + win]
        if seg.size and np.max(np.abs(seg)) > 0.02:
            onsets_with_signal += 1

    print(f"  Detected onsets above threshold: {onsets_with_signal}/3")
    ok = onsets_with_signal == 3
    print(f"  {'PASS' if ok else 'FAIL'}: three distinct onsets")
    return ok


def test_note_change_fallback_no_trigger(vm, sf_id) -> bool:
    """Test 4: trigger NOT wired (inputs[4] = 0xFFFF). Sustained gate, freq
    changes at note boundaries. All three notes must trigger via the
    note-change fallback path."""
    print("\nTest 4: Sequential C4 -> E4 -> G4 WITHOUT trigger (note-change fallback)")

    duration_s = 1.5
    n = (int(duration_s * SR) // BS) * BS
    third = n // 3

    gate = np.ones(n, dtype=np.float32)
    freq = np.empty(n, dtype=np.float32)
    freq[:third] = freq_for_midi(60)
    freq[third:2 * third] = freq_for_midi(64)
    freq[2 * third:] = freq_for_midi(67)
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)

    program = make_sf_program(sf_id, with_trigger=False)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, None, n // BS)

    skip = int(0.05 * SR)
    rms_c = rms(audio[skip:third])
    rms_e = rms(audio[third + skip:2 * third])
    rms_g = rms(audio[2 * third + skip:])

    save_wav("test4_note_change_fallback.wav", audio)
    print(f"  RMS: C4={rms_c:.5f}  E4={rms_e:.5f}  G4={rms_g:.5f}")

    threshold = 1e-3
    ok = rms_c > threshold and rms_e > threshold and rms_g > threshold
    print(f"  {'PASS' if ok else 'FAIL'}: all three notes audible via note-change")
    return ok


def test_gate_gap_between_notes(vm, sf_id) -> bool:
    """Test 5: explicit gate-off gaps between notes, no trigger wired.
    Validates gate-edge detection still works."""
    print("\nTest 5: Notes separated by gate gaps (no trigger)")

    duration_s = 1.8
    n = (int(duration_s * SR) // BS) * BS
    third = n // 3

    gate = np.zeros(n, dtype=np.float32)
    freq = np.empty(n, dtype=np.float32)
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)

    # Three note windows with 50 ms silence between
    gap = int(0.05 * SR)
    for k, midi in enumerate((60, 64, 67)):
        start = k * third
        end = (k + 1) * third - gap
        gate[start:end] = 1.0
        freq[start:end] = freq_for_midi(midi)
        # Fill the gap with the same freq to avoid noise — gate is what matters
        freq[end:end + gap] = freq_for_midi(midi)

    program = make_sf_program(sf_id, with_trigger=False)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, None, n // BS)

    skip = int(0.05 * SR)
    rms_c = rms(audio[skip:third - gap])
    rms_e = rms(audio[third + skip:2 * third - gap])
    rms_g = rms(audio[2 * third + skip:3 * third - gap])

    save_wav("test5_gate_gap.wav", audio)
    print(f"  RMS: C4={rms_c:.5f}  E4={rms_e:.5f}  G4={rms_g:.5f}")
    threshold = 1e-3
    ok = rms_c > threshold and rms_e > threshold and rms_g > threshold
    print(f"  {'PASS' if ok else 'FAIL'}: all three notes audible via gate edges")
    return ok


def test_velocity_scaling(vm, sf_id) -> bool:
    """Test 6: same note at vel=0.2 and vel=0.8 — louder note should produce
    materially more output. The gain ratio in the implementation is linear in
    `vel`, so we expect ~4x amplitude (~12 dB)."""
    print("\nTest 6: Velocity scaling (vel=0.2 vs vel=0.8)")

    duration_s = 1.0
    n = (int(duration_s * SR) // BS) * BS
    half = n // 2

    gate = np.ones(n, dtype=np.float32)
    freq = np.full(n, freq_for_midi(60), dtype=np.float32)
    vel = np.empty(n, dtype=np.float32)
    vel[:half] = 0.2
    vel[half:] = 0.8
    preset = np.zeros(n, dtype=np.float32)
    trig = make_pulse(n, [0, half])

    program = make_sf_program(sf_id, with_trigger=True)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, trig, n // BS)

    skip = int(0.05 * SR)
    rms_quiet = rms(audio[skip:half])
    rms_loud = rms(audio[half + skip:])
    ratio = rms_loud / max(rms_quiet, 1e-9)

    save_wav("test6_velocity_scaling.wav", audio)
    print(f"  RMS: quiet={rms_quiet:.5f}  loud={rms_loud:.5f}  ratio={ratio:.2f}x (expect ~4x)")

    # Allow a wide tolerance: SF zone velocity curves can soften the linear ratio.
    ok = 2.0 < ratio < 8.0 and rms_quiet > 1e-4
    print(f"  {'PASS' if ok else 'FAIL'}: louder velocity is materially louder")
    return ok


def test_vibrato_no_spurious_retrigger(vm, sf_id) -> bool:
    """Edge case 7.1: small pitch wobble within one semitone must NOT cause
    new voice allocations — proxy: output remains a coherent single note,
    no envelope retransients."""
    print("\nTest 7 (Edge 7.1): vibrato within one semitone — no spurious retrigger")

    duration_s = 1.5
    n = (int(duration_s * SR) // BS) * BS
    gate = np.zeros(n, dtype=np.float32)
    gate[int(0.05 * SR):int(1.4 * SR)] = 1.0

    # Wobble: 6 Hz LFO, +/- 30 cents around C4 (well under the semitone boundary)
    t = np.arange(n) / SR
    lfo = np.sin(2 * np.pi * 6.0 * t) * (30.0 / 1200.0)  # cents/1200 = octave fraction
    freq = freq_for_midi(60) * (2.0 ** lfo)
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)

    program = make_sf_program(sf_id, with_trigger=False)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, None, n // BS)

    save_wav("test7_vibrato.wav", audio)

    # If retriggers fired, we'd see periodic transients. Proxy check: the
    # post-attack envelope should be smooth — peak-to-peak deviation across
    # a 50 ms running RMS should stay reasonably bounded.
    win = int(0.05 * SR)
    start = int(0.1 * SR)
    end = int(1.3 * SR)
    seg = audio[start:end]
    rms_curve = np.array([rms(seg[i:i + win]) for i in range(0, len(seg) - win, win // 2)])
    if len(rms_curve) == 0:
        ok = False
    else:
        rel_dev = (rms_curve.max() - rms_curve.min()) / max(rms_curve.mean(), 1e-9)
        print(f"  Sustained RMS variation: {rel_dev:.2f} (lower = smoother)")
        # Vibrato will modulate amplitude a bit (filter sweep, sample interp);
        # we just want it to stay sane — no catastrophic re-attacks.
        ok = rel_dev < 5.0 and rms_curve.mean() > 1e-4
    print(f"  {'PASS' if ok else 'FAIL'}: vibrato sustains as one note")
    return ok


def test_gate_off_at_note_change(vm, sf_id) -> bool:
    """Edge case 7.4: gate drops to 0 at the same sample as a freq change —
    must release, not start a new voice on the new freq."""
    print("\nTest 8 (Edge 7.4): gate-off concurrent with freq change")

    duration_s = 1.0
    n = (int(duration_s * SR) // BS) * BS
    half = n // 2

    gate = np.zeros(n, dtype=np.float32)
    gate[:half] = 1.0  # gate falls at sample `half`
    freq = np.empty(n, dtype=np.float32)
    freq[:half] = freq_for_midi(60)
    freq[half:] = freq_for_midi(67)  # freq changes at sample `half`
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)

    program = make_sf_program(sf_id, with_trigger=False)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, None, n // BS)

    save_wav("test8_gate_off_at_note_change.wav", audio)

    # After gate-off, the C4 voices should be releasing. We allow the release
    # tail to remain audible briefly; what we don't want is a brand-new G4
    # voice attacking. A simple proxy: the post-gate amplitude must be
    # decreasing on average, not rising.
    tail_start = half + int(0.05 * SR)
    early_tail = rms(audio[tail_start:tail_start + int(0.1 * SR)])
    late_tail = rms(audio[tail_start + int(0.4 * SR):tail_start + int(0.5 * SR)])
    print(f"  Tail RMS early={early_tail:.5f}, late={late_tail:.5f}")
    ok = late_tail < early_tail
    print(f"  {'PASS' if ok else 'FAIL'}: tail decays (no new voice attack)")
    return ok


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    fixture = find_fixture()
    if not fixture:
        print("SKIP: No SoundFont fixture found under web/static/soundfonts/.")
        print("      Place a TimGM6mb.sf3 there to run this test.")
        return 0

    print(f"Using fixture: {fixture}")
    vm = cedar.VM()
    vm.set_sample_rate(SR)

    sf_id = vm.soundfont_load_from_file("gm", fixture)
    if sf_id < 0:
        print(f"FAIL: soundfont_load_from_file returned {sf_id}")
        return 1
    print(f"Loaded SoundFont (id={sf_id}), preset count = "
          f"{vm.soundfont_count()}")

    long_trace = "--soak" in sys.argv

    results = []
    for fn in (
        lambda: test_single_note(vm, sf_id),
        lambda: test_sequential_notes_with_trigger(vm, sf_id, long_trace=long_trace),
        lambda: test_same_note_repeat_with_trigger(vm, sf_id),
        lambda: test_note_change_fallback_no_trigger(vm, sf_id),
        lambda: test_gate_gap_between_notes(vm, sf_id),
        lambda: test_velocity_scaling(vm, sf_id),
        lambda: test_vibrato_no_spurious_retrigger(vm, sf_id),
        lambda: test_gate_off_at_note_change(vm, sf_id),
    ):
        # Reset between tests so each starts with a fresh state pool.
        vm.reset()
        results.append(fn())

    passed = sum(results)
    total = len(results)
    print(f"\n=== {passed}/{total} tests passed ===")
    print(f"WAV files: {OUT}")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())

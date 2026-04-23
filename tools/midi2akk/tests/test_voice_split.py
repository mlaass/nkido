from midi2akk.midi_reader import Note, TrackIR
from midi2akk.voice_split import split_track


def _mk_track(*notes: tuple[int, int, int]) -> TrackIR:
    """notes = list of (start_tick, duration, midi)."""
    return TrackIR(
        index=0, name="t", channel=0, program=0, is_drum=False,
        notes=[Note(start_tick=s, duration_ticks=d, midi_note=m,
                    velocity=100, channel=0) for s, d, m in notes],
    )


def test_monophonic_stays_one_voice() -> None:
    track = _mk_track((0, 100, 60), (100, 100, 62), (200, 100, 64))
    r = split_track(track, max_voices=4)
    assert len(r.voices) == 1
    assert [n.midi_note for n in r.voices[0].notes] == [60, 62, 64]
    assert r.dropped_count == 0


def test_four_note_chord_splits_into_four_voices() -> None:
    # Four notes all starting at tick 0 — none can share a voice.
    track = _mk_track((0, 100, 60), (0, 100, 64), (0, 100, 67), (0, 100, 71))
    r = split_track(track, max_voices=4)
    assert len(r.voices) == 4
    assert r.dropped_count == 0


def test_exceeding_max_voices_drops_notes() -> None:
    track = _mk_track(*[(0, 100, m) for m in [60, 62, 64, 66, 68]])
    r = split_track(track, max_voices=3)
    assert len(r.voices) == 3
    assert r.dropped_count == 2


def test_reuses_freed_voice_for_later_notes() -> None:
    # Two overlapping notes, then a third that fits in voice 0 after it frees.
    track = _mk_track((0, 100, 60), (50, 100, 64), (200, 100, 67))
    r = split_track(track, max_voices=4)
    assert len(r.voices) == 2
    # voice 0 should carry the 60 and the 67 (the 50..150 overlap pushes 64 to voice 1)
    v0_pitches = sorted(n.midi_note for n in r.voices[0].notes)
    assert v0_pitches == [60, 67]

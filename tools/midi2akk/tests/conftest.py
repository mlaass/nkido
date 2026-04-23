"""Shared fixtures — tiny MIDI files built in memory."""

from __future__ import annotations

from pathlib import Path

import mido
import pytest

TICKS_PER_BEAT = 480  # standard


def _write_mary_little_lamb(path: Path) -> None:
    """Single monophonic track: E4 D4 C4 D4 E4 E4 E4 (eighth notes-ish)."""
    mid = mido.MidiFile(ticks_per_beat=TICKS_PER_BEAT)
    tr = mido.MidiTrack()
    mid.tracks.append(tr)
    tr.append(mido.MetaMessage("track_name", name="lead", time=0))
    tr.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(120), time=0))
    tr.append(mido.Message("program_change", channel=0, program=0, time=0))
    for note in [64, 62, 60, 62, 64, 64, 64]:
        tr.append(mido.Message("note_on", note=note, velocity=100, time=0, channel=0))
        tr.append(mido.Message("note_off", note=note, velocity=0, time=TICKS_PER_BEAT, channel=0))
    mid.save(str(path))


def _write_chord(path: Path) -> None:
    """Four simultaneous notes at tick 0, all held one beat: C E G B."""
    mid = mido.MidiFile(ticks_per_beat=TICKS_PER_BEAT)
    tr = mido.MidiTrack()
    mid.tracks.append(tr)
    tr.append(mido.MetaMessage("track_name", name="chord", time=0))
    tr.append(mido.Message("program_change", channel=0, program=0, time=0))
    for note in [60, 64, 67, 71]:
        tr.append(mido.Message("note_on", note=note, velocity=100, time=0, channel=0))
    # Now release them all after one beat (only the first gets tick delta, rest are 0).
    tr.append(mido.Message("note_off", note=60, velocity=0, time=TICKS_PER_BEAT, channel=0))
    for note in [64, 67, 71]:
        tr.append(mido.Message("note_off", note=note, velocity=0, time=0, channel=0))
    mid.save(str(path))


def _write_drums(path: Path) -> None:
    """Kick on 1/3, snare on 2/4 — basic backbeat over one bar of 4/4."""
    mid = mido.MidiFile(ticks_per_beat=TICKS_PER_BEAT)
    tr = mido.MidiTrack()
    mid.tracks.append(tr)
    tr.append(mido.MetaMessage("track_name", name="drums", time=0))
    tr.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(120), time=0))
    # Channel 9 = drum channel in mido (0-indexed).
    seq = [(36, 0), (38, TICKS_PER_BEAT),
           (36, TICKS_PER_BEAT), (38, TICKS_PER_BEAT)]
    for note, dt in seq:
        tr.append(mido.Message("note_on", note=note, velocity=110, time=dt, channel=9))
        tr.append(mido.Message("note_off", note=note, velocity=0, time=120, channel=9))
    mid.save(str(path))


@pytest.fixture
def mary_midi(tmp_path: Path) -> Path:
    p = tmp_path / "mary.mid"
    _write_mary_little_lamb(p)
    return p


@pytest.fixture
def chord_midi(tmp_path: Path) -> Path:
    p = tmp_path / "chord.mid"
    _write_chord(p)
    return p


@pytest.fixture
def drums_midi(tmp_path: Path) -> Path:
    p = tmp_path / "drums.mid"
    _write_drums(p)
    return p

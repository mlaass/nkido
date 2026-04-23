"""Parse a MIDI file into a simple internal representation.

We collapse mid-song tempo and meter changes to their initial values — Akkado
has a single global BPM and no tempo-automation path. Document this as a
known limitation in the README.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import mido

DRUM_CHANNEL = 9  # MIDI channel 10 (0-indexed)


@dataclass
class Note:
    start_tick: int
    duration_ticks: int
    midi_note: int   # 0..127
    velocity: int    # 0..127
    channel: int


@dataclass
class TrackIR:
    index: int
    name: str
    channel: int            # dominant channel seen on this track
    program: int            # last program_change on this channel (default 0)
    is_drum: bool
    notes: list[Note] = field(default_factory=list)


@dataclass
class SongIR:
    source_path: str
    ticks_per_beat: int
    bpm: float
    time_sig_num: int
    time_sig_den: int
    tracks: list[TrackIR]


def _initial_tempo(midi: mido.MidiFile) -> float:
    """Scan all tracks for the first set_tempo. Default 120 BPM."""
    for track in midi.tracks:
        for msg in track:
            if msg.type == "set_tempo":
                return mido.tempo2bpm(msg.tempo)
    return 120.0


def _initial_time_signature(midi: mido.MidiFile) -> tuple[int, int]:
    for track in midi.tracks:
        for msg in track:
            if msg.type == "time_signature":
                return msg.numerator, msg.denominator
    return 4, 4


def _dominant_channel(notes: list[Note]) -> int:
    if not notes:
        return 0
    counts: dict[int, int] = {}
    for n in notes:
        counts[n.channel] = counts.get(n.channel, 0) + 1
    return max(counts.items(), key=lambda kv: kv[1])[0]


def _read_track(track: mido.MidiTrack, index: int) -> TrackIR | None:
    """Convert note_on/note_off pairs to Note records. Return None if empty."""
    abs_tick = 0
    # Map (channel, note) -> (start_tick, velocity)
    active: dict[tuple[int, int], tuple[int, int]] = {}
    notes: list[Note] = []
    # Track the last program_change per channel
    program_per_channel: dict[int, int] = {}
    name: str | None = None

    for msg in track:
        abs_tick += msg.time
        if msg.type == "track_name":
            name = msg.name.strip() or None
        elif msg.type == "program_change":
            program_per_channel[msg.channel] = msg.program
        elif msg.type == "note_on" and msg.velocity > 0:
            active[(msg.channel, msg.note)] = (abs_tick, msg.velocity)
        elif msg.type == "note_off" or (msg.type == "note_on" and msg.velocity == 0):
            key = (msg.channel, msg.note)
            if key in active:
                start_tick, vel = active.pop(key)
                duration = max(1, abs_tick - start_tick)
                notes.append(Note(
                    start_tick=start_tick,
                    duration_ticks=duration,
                    midi_note=msg.note,
                    velocity=vel,
                    channel=msg.channel,
                ))

    if not notes:
        return None

    channel = _dominant_channel(notes)
    program = program_per_channel.get(channel, 0)
    is_drum = (channel == DRUM_CHANNEL)
    notes.sort(key=lambda n: (n.start_tick, n.midi_note))

    return TrackIR(
        index=index,
        name=name or f"track_{index}",
        channel=channel,
        program=program,
        is_drum=is_drum,
        notes=notes,
    )


def read_midi(path: str | Path) -> SongIR:
    path = Path(path)
    midi = mido.MidiFile(str(path))

    tracks: list[TrackIR] = []
    for i, track in enumerate(midi.tracks):
        ir = _read_track(track, i)
        if ir is not None:
            tracks.append(ir)

    return SongIR(
        source_path=str(path),
        ticks_per_beat=midi.ticks_per_beat,
        bpm=_initial_tempo(midi),
        time_sig_num=_initial_time_signature(midi)[0],
        time_sig_den=_initial_time_signature(midi)[1],
        tracks=tracks,
    )

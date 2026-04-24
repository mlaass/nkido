"""Split a polyphonic MIDI track into monophonic voice streams.

Akkado patterns emit one event at a time (see docs/prd-polyphony-system.md).
For chords or overlapping notes we must spread them across multiple
`pat(...) |> soundfont(...)` lines.

Algorithm: greedy earliest-free-voice allocation, capped at max_voices.
Notes that can't fit are dropped and counted for a warning.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .midi_reader import Note, TrackIR


@dataclass
class VoiceIR:
    voice_index: int
    notes: list[Note] = field(default_factory=list)


@dataclass
class SplitResult:
    voices: list[VoiceIR]
    dropped_count: int


def split_track(track: TrackIR, max_voices: int = 4) -> SplitResult:
    if max_voices < 1:
        raise ValueError("max_voices must be >= 1")

    # voice_end_tick[i] = absolute tick at which voice i becomes free.
    voice_end_tick: list[int] = []
    voices: list[VoiceIR] = []
    dropped = 0

    for note in sorted(track.notes, key=lambda n: n.start_tick):
        placed = False
        # Prefer the earliest-freed voice that's free by this note's start.
        for i, end in enumerate(voice_end_tick):
            if end <= note.start_tick:
                voices[i].notes.append(note)
                voice_end_tick[i] = note.start_tick + note.duration_ticks
                placed = True
                break
        if placed:
            continue

        if len(voices) < max_voices:
            v = VoiceIR(voice_index=len(voices), notes=[note])
            voices.append(v)
            voice_end_tick.append(note.start_tick + note.duration_ticks)
        else:
            dropped += 1

    return SplitResult(voices=voices, dropped_count=dropped)

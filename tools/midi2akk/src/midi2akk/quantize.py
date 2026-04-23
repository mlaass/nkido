"""Quantize a monophonic voice to a time grid.

Grid: `subdivision` per beat (e.g. 16th notes = 4 slots/beat). We snap each
note's start and end to the nearest slot, then render a per-slot array of
Note / Hold / Rest tokens suitable for mini-notation emission.
"""

from __future__ import annotations

from .mini_notation import Slot
from .voice_split import VoiceIR


def slots_per_beat(subdivision: int) -> int:
    """subdivision is notated value (4=quarter, 8=eighth, 16=sixteenth...).

    A 16th-note subdivision means 4 slots per beat (beat = quarter note).
    """
    if subdivision < 4 or (subdivision & (subdivision - 1)) != 0:
        raise ValueError(
            f"subdivision must be a power of two ≥ 4 (got {subdivision})"
        )
    return subdivision // 4


def _snap(tick: int, ticks_per_slot: float) -> int:
    return int(round(tick / ticks_per_slot))


def quantize_voice(
    voice: VoiceIR,
    ticks_per_beat: int,
    subdivision: int,
    slots_per_bar: int,
    total_bars: int,
) -> tuple[list[Slot], int]:
    """Return (slot array, collisions dropped).

    The slot array spans exactly `total_bars * slots_per_bar` slots so every
    voice aligns with every other voice when summed.
    """
    spb = slots_per_beat(subdivision)
    ticks_per_slot = ticks_per_beat / spb
    total_slots = total_bars * slots_per_bar

    slots: list[Slot] = [Slot.rest() for _ in range(total_slots)]
    collisions = 0

    for note in voice.notes:
        start_slot = _snap(note.start_tick, ticks_per_slot)
        end_slot = _snap(note.start_tick + note.duration_ticks, ticks_per_slot)
        if end_slot <= start_slot:
            end_slot = start_slot + 1  # ensure at least one slot

        if start_slot >= total_slots:
            continue
        end_slot = min(end_slot, total_slots)

        if slots[start_slot].kind.name != "REST":
            collisions += 1
            continue

        slots[start_slot] = Slot.note(note.midi_note)
        for i in range(start_slot + 1, end_slot):
            if slots[i].kind.name == "REST":
                slots[i] = Slot.hold()
            # If not rest, another voice's onset is already here — leave it alone.

    return slots, collisions


def total_bars_for_song(
    last_end_tick: int,
    ticks_per_beat: int,
    beats_per_bar: int,
) -> int:
    """Number of bars needed to cover up to last_end_tick (inclusive)."""
    if last_end_tick <= 0:
        return 1
    ticks_per_bar = ticks_per_beat * beats_per_bar
    return max(1, -(-last_end_tick // ticks_per_bar))  # ceil div

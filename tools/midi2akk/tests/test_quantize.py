from midi2akk.midi_reader import Note
from midi2akk.mini_notation import SlotKind
from midi2akk.quantize import (
    quantize_voice,
    slots_per_beat,
    total_bars_for_song,
)
from midi2akk.voice_split import VoiceIR


def test_slots_per_beat() -> None:
    assert slots_per_beat(4) == 1    # quarter-note grid
    assert slots_per_beat(8) == 2    # eighth-note grid
    assert slots_per_beat(16) == 4   # sixteenth-note grid
    assert slots_per_beat(32) == 8


def test_total_bars_rounds_up() -> None:
    # 480 ticks/beat, 4 beats/bar = 1920 ticks/bar.
    # A song that ends at tick 1921 should need 2 bars.
    assert total_bars_for_song(1920, 480, 4) == 1
    assert total_bars_for_song(1921, 480, 4) == 2
    assert total_bars_for_song(0, 480, 4) == 1


def test_quantize_monophonic_eighths() -> None:
    # 4 quarter-note-length events at 16th-note subdivision (4 slots/beat).
    # Each event: 480 ticks at 480 ticks/beat = exactly one beat = 4 slots.
    notes = [Note(start_tick=i * 480, duration_ticks=480,
                  midi_note=60 + i, velocity=100, channel=0) for i in range(4)]
    voice = VoiceIR(voice_index=0, notes=notes)
    slots, collisions = quantize_voice(
        voice,
        ticks_per_beat=480,
        subdivision=16,
        slots_per_bar=16,
        total_bars=1,
    )
    assert collisions == 0
    assert len(slots) == 16
    # Onsets at slots 0, 4, 8, 12
    assert slots[0].kind is SlotKind.NOTE and slots[0].midi == 60
    assert slots[4].kind is SlotKind.NOTE and slots[4].midi == 61
    assert slots[8].kind is SlotKind.NOTE and slots[8].midi == 62
    assert slots[12].kind is SlotKind.NOTE and slots[12].midi == 63
    # Slots between onsets should be HOLD
    for i in (1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15):
        assert slots[i].kind is SlotKind.HOLD, f"slot {i} was {slots[i]}"


def test_quantize_collision_counts_but_keeps_first() -> None:
    # Two onsets on the same slot — second is dropped.
    notes = [
        Note(start_tick=0, duration_ticks=120, midi_note=60, velocity=100, channel=0),
        Note(start_tick=1, duration_ticks=120, midi_note=72, velocity=100, channel=0),
    ]
    voice = VoiceIR(voice_index=0, notes=notes)
    slots, collisions = quantize_voice(
        voice,
        ticks_per_beat=480,
        subdivision=16,
        slots_per_bar=16,
        total_bars=1,
    )
    assert collisions == 1
    assert slots[0].kind is SlotKind.NOTE and slots[0].midi == 60


def test_quantize_short_note_occupies_one_slot() -> None:
    notes = [Note(start_tick=0, duration_ticks=10, midi_note=60, velocity=100, channel=0)]
    voice = VoiceIR(voice_index=0, notes=notes)
    slots, _ = quantize_voice(voice, ticks_per_beat=480, subdivision=16,
                              slots_per_bar=16, total_bars=1)
    assert slots[0].kind is SlotKind.NOTE
    assert slots[1].kind is SlotKind.REST

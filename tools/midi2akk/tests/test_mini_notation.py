from midi2akk.mini_notation import (
    Slot,
    midi_to_name,
    slots_to_pattern,
)


def test_midi_to_name_standard_pitches() -> None:
    assert midi_to_name(60) == "c4"
    assert midi_to_name(61) == "c#4"
    assert midi_to_name(69) == "a4"
    assert midi_to_name(70) == "a#4"
    assert midi_to_name(72) == "c5"


def test_midi_to_name_extremes() -> None:
    assert midi_to_name(0) == "c-1"
    assert midi_to_name(127) == "g9"


def test_midi_to_name_out_of_range() -> None:
    import pytest
    with pytest.raises(ValueError):
        midi_to_name(-1)
    with pytest.raises(ValueError):
        midi_to_name(128)


def test_slots_to_pattern_single_bar() -> None:
    # 4 slots per bar, one note + 3 rests
    slots = [Slot.note(60), Slot.rest(), Slot.rest(), Slot.rest()]
    assert slots_to_pattern(slots, slots_per_bar=4) == "[c4 ~ ~ ~]"


def test_slots_to_pattern_holds_and_rest() -> None:
    # c4 held 2 slots, then rest, then e4
    slots = [Slot.note(60), Slot.hold(), Slot.rest(), Slot.note(64)]
    assert slots_to_pattern(slots, slots_per_bar=4) == "[c4 _ ~ e4]"


def test_slots_to_pattern_multiple_bars() -> None:
    slots = [Slot.note(60), Slot.rest(), Slot.note(64), Slot.rest()]
    # Two bars of 2 slots each
    assert slots_to_pattern(slots, slots_per_bar=2) == "[c4 ~] [e4 ~]"


def test_slots_to_pattern_pads_trailing_rests() -> None:
    # Only 3 slots given but bar size 4 — pad with rest
    slots = [Slot.note(60), Slot.hold(), Slot.note(62)]
    assert slots_to_pattern(slots, slots_per_bar=4) == "[c4 _ d4 ~]"


def test_slots_to_pattern_empty() -> None:
    assert slots_to_pattern([], slots_per_bar=4) == "~"

"""MIDI → Akkado mini-notation.

Pitch syntax per docs/mini-notation-reference.md:
- Lowercase letter + optional accidental (# or b) + octave (0-9)
- c4 = MIDI 60 (middle C)
- No raw MIDI number syntax (e.g. n60) — always emit pitch names.
"""

from dataclasses import dataclass
from enum import Enum, auto

NOTE_NAMES = ["c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"]


def midi_to_name(midi: int) -> str:
    """Convert MIDI note number (0-127) to Akkado pitch string.

    MIDI 60 -> "c4", MIDI 61 -> "c#4", MIDI 0 -> "c-1", MIDI 127 -> "g9".
    """
    if midi < 0 or midi > 127:
        raise ValueError(f"MIDI note out of range: {midi}")
    return f"{NOTE_NAMES[midi % 12]}{midi // 12 - 1}"


class SlotKind(Enum):
    NOTE = auto()
    HOLD = auto()
    REST = auto()


@dataclass
class Slot:
    kind: SlotKind
    midi: int = 0  # only meaningful for NOTE

    @staticmethod
    def note(midi: int) -> "Slot":
        return Slot(SlotKind.NOTE, midi)

    @staticmethod
    def hold() -> "Slot":
        return Slot(SlotKind.HOLD)

    @staticmethod
    def rest() -> "Slot":
        return Slot(SlotKind.REST)


def slot_token(slot: Slot) -> str:
    if slot.kind is SlotKind.NOTE:
        return midi_to_name(slot.midi)
    if slot.kind is SlotKind.HOLD:
        return "_"
    return "~"


def slots_to_pattern(slots: list[Slot], slots_per_bar: int) -> str:
    """Render a slot array as a mini-notation pattern string.

    Each bar is wrapped in [...] for equal subdivision within the bar.
    The whole pattern is a space-separated sequence of bar groups.

    A pattern with N bars spans N cycles (since one [...] group = one cycle).
    """
    if slots_per_bar <= 0:
        raise ValueError("slots_per_bar must be positive")
    if not slots:
        return "~"

    bars: list[str] = []
    for start in range(0, len(slots), slots_per_bar):
        bar_slots = slots[start : start + slots_per_bar]
        while len(bar_slots) < slots_per_bar:
            bar_slots.append(Slot.rest())
        tokens = [slot_token(s) for s in bar_slots]
        bars.append("[" + " ".join(tokens) + "]")
    return " ".join(bars)

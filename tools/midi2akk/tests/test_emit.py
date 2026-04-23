from pathlib import Path

from midi2akk.emit import convert
from midi2akk.midi_reader import read_midi


def test_monophonic_melody_emits_single_voice(mary_midi: Path) -> None:
    song = read_midi(mary_midi)
    out = convert(song, subdivision=16)

    assert "bpm = 120" in out
    assert "soundfont(%, \"GM.sf3\", 0)" in out  # piano preset
    # Emitted pattern starts at E4 per fixture
    assert "e4" in out
    # Single-voice shortcut: mix helper returns just the name (no `* (1/N)`).
    assert "* (1.0 /" not in out
    assert "lead = pat(" in out
    assert "lead |> out(%, %)" in out


def test_chord_splits_into_four_voices(chord_midi: Path) -> None:
    song = read_midi(chord_midi)
    out = convert(song, subdivision=16, max_voices=4)

    assert "chord_v1" in out
    assert "chord_v2" in out
    assert "chord_v3" in out
    assert "chord_v4" in out
    # Mix line scales by 1/4
    assert "* (1.0 / 4)" in out
    # Pattern should contain c4, e4, g4, b4 across the four voices
    for name in ("c4", "e4", "g4", "b4"):
        assert name in out, f"missing {name} in emitted source"


def test_drum_track_uses_percussion_preset(drums_midi: Path) -> None:
    song = read_midi(drums_midi)
    out = convert(song, subdivision=16)

    assert "soundfont(%, \"GM.sf3\", 128)" in out
    # MIDI 36 -> c2 (kick), 38 -> d2 (snare)
    assert "c2" in out
    assert "d2" in out
    assert "// drums" in out.lower()


def test_osc_backend_replaces_soundfont(mary_midi: Path) -> None:
    song = read_midi(mary_midi)
    out = convert(song, instrument="osc")

    assert "soundfont(" not in out
    assert 'osc("saw"' in out
    assert "adsr(" in out

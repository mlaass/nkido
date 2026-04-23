"""CLI entry point for the midi2akk converter."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .emit import convert
from .midi_reader import read_midi


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="midi2akk",
        description="Convert a MIDI file into an Akkado program. "
                    "By default each track is rendered through the GM soundfont.",
    )
    parser.add_argument("input", type=Path, help="Input .mid file")
    parser.add_argument(
        "-o", "--output", type=Path, default=None,
        help="Output .akk path (defaults to <input>.akk, or stdout if '-')",
    )
    parser.add_argument(
        "--subdivision", type=int, choices=[4, 8, 16, 32], default=16,
        help="Timing grid: 16 = 16th-note snap (default)",
    )
    parser.add_argument(
        "--max-voices", type=int, default=4,
        help="Cap per-track voice count for polyphonic MIDI (default: 4)",
    )
    parser.add_argument(
        "--soundfont", type=str, default="GM.sf3",
        help='Soundfont filename to reference (default: "GM.sf3"). '
             "Must be loaded by the Akkado host at runtime.",
    )
    parser.add_argument(
        "--instrument", choices=["soundfont", "osc"], default="soundfont",
        help="Backend: 'soundfont' uses soundfont() (default, GM-flavored), "
             "'osc' uses a saw+ADSR synth voice as a zero-dependency fallback",
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="Suppress progress messages on stderr",
    )
    args = parser.parse_args(argv)

    if not args.input.exists():
        print(f"error: input file not found: {args.input}", file=sys.stderr)
        return 2

    song = read_midi(args.input)
    if not args.quiet:
        print(
            f"midi2akk: {args.input.name} — {len(song.tracks)} non-empty track(s), "
            f"{song.bpm:.1f} bpm, {song.time_sig_num}/{song.time_sig_den}",
            file=sys.stderr,
        )

    source = convert(
        song,
        subdivision=args.subdivision,
        max_voices=args.max_voices,
        soundfont=args.soundfont,
        instrument=args.instrument,
    )

    if args.output is None:
        out_path = args.input.with_suffix(".akk")
        out_path.write_text(source)
        if not args.quiet:
            print(f"midi2akk: wrote {out_path}", file=sys.stderr)
    elif str(args.output) == "-":
        sys.stdout.write(source)
    else:
        args.output.write_text(source)
        if not args.quiet:
            print(f"midi2akk: wrote {args.output}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())

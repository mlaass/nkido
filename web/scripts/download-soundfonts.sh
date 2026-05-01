#!/bin/bash
# Download default SoundFonts for the web app
#   TimGM6mb       - compact GM SoundFont (~5.7MB SF2, ~2.6MB SF3) - default "gm"
#   FluidR3Mono_GM - medium GM SoundFont (~14MB SF3) - optional "gm_medium"
#   MuseScore      - high-quality GM SoundFont (~39MB SF3) - optional "gm_large"
# If sf3convert is available, SF2 files are compressed to SF3

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$SCRIPT_DIR/../static/soundfonts"

# --minimal: download only TimGM6mb (the default "gm" bank). Used by
# Netlify CI to keep build size/time down. Local dev gets the full set.
MINIMAL=false
if [ "$1" = "--minimal" ]; then
    MINIMAL=true
fi

mkdir -p "$DEST_DIR"

HAS_SF3CONVERT=false
if command -v sf3convert &> /dev/null; then
    HAS_SF3CONVERT=true
fi

# --- TimGM6mb (default "gm") ---

TIMGM_SF2_URL="https://raw.githubusercontent.com/arbruijn/TimGM6mb/master/TimGM6mb.sf2"
TIMGM_SF2="$DEST_DIR/TimGM6mb.sf2"
TIMGM_SF3="$DEST_DIR/TimGM6mb.sf3"

if [ -f "$TIMGM_SF3" ]; then
    echo "TimGM6mb.sf3 already exists, skipping"
elif [ -f "$TIMGM_SF2" ]; then
    echo "TimGM6mb.sf2 already exists, skipping download"
else
    echo "Downloading TimGM6mb.sf2..."
    curl -L -o "$TIMGM_SF2" "$TIMGM_SF2_URL"
    echo "Done: $(du -h "$TIMGM_SF2" | cut -f1)"
fi

if [ -f "$TIMGM_SF2" ] && $HAS_SF3CONVERT; then
    echo "Converting TimGM6mb to SF3..."
    sf3convert -z "$TIMGM_SF2" "$TIMGM_SF3"
    rm "$TIMGM_SF2"
    echo "Done: $(du -h "$TIMGM_SF3" | cut -f1)"
fi

if ! $MINIMAL; then
    # --- FluidR3Mono_GM (optional "gm_medium") ---

    FLUID_SF3_URL="https://github.com/musescore/MuseScore/raw/2.1/share/sound/FluidR3Mono_GM.sf3"
    FLUID_SF3="$DEST_DIR/FluidR3Mono_GM.sf3"

    if [ -f "$FLUID_SF3" ]; then
        echo "FluidR3Mono_GM.sf3 already exists, skipping"
    else
        echo "Downloading FluidR3Mono_GM.sf3..."
        curl -L -o "$FLUID_SF3" "$FLUID_SF3_URL"
        echo "Done: $(du -h "$FLUID_SF3" | cut -f1)"
    fi

    # --- MuseScore_General (optional "gm_large") ---

    MUSESCORE_SF3_URL="https://ftp.osuosl.org/pub/musescore/soundfont/MuseScore_General/MuseScore_General.sf3"
    MUSESCORE_SF3="$DEST_DIR/MuseScore_General.sf3"

    if [ -f "$MUSESCORE_SF3" ]; then
        echo "MuseScore_General.sf3 already exists, skipping"
    else
        echo "Downloading MuseScore_General.sf3..."
        curl -L -o "$MUSESCORE_SF3" "$MUSESCORE_SF3_URL"
        echo "Done: $(du -h "$MUSESCORE_SF3" | cut -f1)"
    fi
else
    echo "Minimal mode: skipping FluidR3Mono_GM and MuseScore_General"
fi

# --- License ---

cat > "$DEST_DIR/LICENSE.md" << 'EOF'
# SoundFont Licenses

## TimGM6mb
A General MIDI SoundFont by Tim Brechbill.
Originally bundled with MuseScore 1.x.
License: GPL-2
Source: https://github.com/arbruijn/TimGM6mb

## FluidR3Mono_GM
Mono version of Fluid (R3) General MIDI SoundFont.
Copyright (c) 2000-2002, 2008 Frank Wen. Mono conversion by Michael Cowgill.
License: MIT
Source: https://github.com/musescore/MuseScore/tree/2.1/share/sound

## MuseScore_General
General MIDI SoundFont based on FluidR3 by Frank Wen.
Adapted by S. Christian Collins.
License: MIT
Source: https://ftp.osuosl.org/pub/musescore/soundfont/MuseScore_General/
EOF

if ! $HAS_SF3CONVERT; then
    echo ""
    echo "Note: sf3convert not found, TimGM6mb kept as SF2"
    echo "Install with: sudo apt install sf3convert"
fi

echo "SoundFont setup complete"

#!/bin/bash
# Download default SoundFonts for the web app
# MuseScore_General.sf3 - General MIDI SoundFont (MIT license, ~38MB)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$SCRIPT_DIR/../static/soundfonts"

SF3_URL="https://ftp.osuosl.org/pub/musescore/soundfont/MuseScore_General/MuseScore_General.sf3"
SF3_FILE="$DEST_DIR/MuseScore_General.sf3"

LICENSE_URL="https://ftp.osuosl.org/pub/musescore/soundfont/MuseScore_General/MuseScore_General_License.md"
LICENSE_FILE="$DEST_DIR/LICENSE.md"

mkdir -p "$DEST_DIR"

if [ -f "$SF3_FILE" ]; then
    echo "MuseScore_General.sf3 already exists, skipping download"
else
    echo "Downloading MuseScore_General.sf3..."
    curl -L -o "$SF3_FILE" "$SF3_URL"
    echo "Done: $(du -h "$SF3_FILE" | cut -f1)"
fi

if [ -f "$LICENSE_FILE" ]; then
    echo "LICENSE.md already exists, skipping download"
else
    echo "Downloading license..."
    curl -L -o "$LICENSE_FILE" "$LICENSE_URL"
    echo "Done"
fi

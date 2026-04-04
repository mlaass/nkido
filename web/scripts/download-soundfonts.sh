#!/bin/bash
# Download default SoundFonts for the web app
# TimGM6mb - General MIDI SoundFont by Tim Brechbill (GPL-2, ~5.7MB SF2)
# If sf3convert is available, compresses to SF3 (~1-2MB)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$SCRIPT_DIR/../static/soundfonts"

SF2_URL="https://raw.githubusercontent.com/arbruijn/TimGM6mb/master/TimGM6mb.sf2"
SF2_FILE="$DEST_DIR/TimGM6mb.sf2"
SF3_FILE="$DEST_DIR/TimGM6mb.sf3"

mkdir -p "$DEST_DIR"

# Download SF2 if neither SF2 nor SF3 exists
if [ -f "$SF3_FILE" ]; then
    echo "TimGM6mb.sf3 already exists, skipping download"
    exit 0
elif [ -f "$SF2_FILE" ]; then
    echo "TimGM6mb.sf2 already exists, skipping download"
else
    echo "Downloading TimGM6mb.sf2..."
    curl -L -o "$SF2_FILE" "$SF2_URL"
    echo "Done: $(du -h "$SF2_FILE" | cut -f1)"
fi

# Convert to SF3 if sf3convert is available
if [ -f "$SF2_FILE" ] && command -v sf3convert &> /dev/null; then
    echo "Converting to SF3..."
    sf3convert -z "$SF2_FILE" "$SF3_FILE"
    rm "$SF2_FILE"
    echo "Done: $(du -h "$SF3_FILE" | cut -f1)"
elif [ -f "$SF2_FILE" ]; then
    echo "sf3convert not found, keeping SF2 format"
    echo "Install with: sudo apt install sf3convert"
fi

# Write license
cat > "$DEST_DIR/LICENSE.md" << 'EOF'
# TimGM6mb SoundFont

A General MIDI SoundFont by Tim Brechbill.
Originally bundled with MuseScore 1.x.

License: GPL-2

Source: https://github.com/arbruijn/TimGM6mb
EOF

echo "SoundFont setup complete"

#!/bin/bash
set -euo pipefail

# Extracts the body of a versioned section from CHANGELOG.md.
#
# Usage: extract-changelog.sh <version>
#   e.g. extract-changelog.sh 0.1.2
#
# Prints the lines between "## [<version>]" and the next "## [" heading,
# trimming surrounding blank lines. Exits non-zero if the section is missing
# or empty, so callers (release workflow, bump-version.sh) fail loudly when
# a release is missing notes.

if [ $# -ne 1 ]; then
    echo "Usage: $0 <version>" >&2
    exit 1
fi

VERSION="$1"
CHANGELOG="$(dirname "$0")/../CHANGELOG.md"

if [ ! -f "$CHANGELOG" ]; then
    echo "Error: CHANGELOG.md not found at $CHANGELOG" >&2
    exit 1
fi

# awk extracts lines after the matching version heading up to (but not
# including) the next "## [" heading. Headings themselves are dropped.
BODY=$(awk -v ver="$VERSION" '
    BEGIN { in_section = 0 }
    /^## \[/ {
        if (in_section) { exit }
        if ($0 ~ "^## \\[" ver "\\]") { in_section = 1; next }
    }
    in_section { print }
' "$CHANGELOG")

# Trim leading and trailing blank lines.
BODY=$(printf '%s\n' "$BODY" | sed -e '/./,$!d' | tac | sed -e '/./,$!d' | tac)

if [ -z "$BODY" ]; then
    echo "Error: no entry for version $VERSION found in CHANGELOG.md" >&2
    exit 1
fi

printf '%s\n' "$BODY"

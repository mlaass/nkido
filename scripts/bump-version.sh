#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

# --- Argument validation ---
if [ $# -ne 1 ]; then
    echo "Usage: $0 <major|minor|patch>"
    exit 1
fi

BUMP_TYPE="$1"
if [[ "$BUMP_TYPE" != "major" && "$BUMP_TYPE" != "minor" && "$BUMP_TYPE" != "patch" ]]; then
    echo "Error: argument must be 'major', 'minor', or 'patch'"
    exit 1
fi

# --- Read current version from VERSION file ---
if [ ! -f VERSION ]; then
    echo "Error: VERSION file not found"
    exit 1
fi

CURRENT_VERSION=$(head -1 VERSION | tr -d '[:space:]')
if ! [[ "$CURRENT_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: VERSION file does not contain a valid semver: '$CURRENT_VERSION'"
    exit 1
fi

IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VERSION"
echo "Current version: $CURRENT_VERSION"

# --- Compute new version ---
case "$BUMP_TYPE" in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    patch)
        PATCH=$((PATCH + 1))
        ;;
esac

NEW_VERSION="$MAJOR.$MINOR.$PATCH"
echo "New version: $NEW_VERSION ($BUMP_TYPE bump)"

# --- Check for clean working tree ---
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "Error: working tree is not clean. Commit or stash changes first."
    exit 1
fi

# --- Check tag doesn't already exist ---
if git rev-parse "v$NEW_VERSION" >/dev/null 2>&1; then
    echo "Error: tag v$NEW_VERSION already exists"
    exit 1
fi

# --- Check CHANGELOG has an entry for the new version ---
# The release workflow extracts release notes from CHANGELOG.md by version
# heading; bumping without an entry would produce an empty GitHub Release.
if ! grep -qE "^## \[$NEW_VERSION\]" CHANGELOG.md; then
    echo "Error: CHANGELOG.md has no '## [$NEW_VERSION]' section."
    echo "       Run the /update-changelog skill (or edit CHANGELOG.md by hand)"
    echo "       to add release notes for $NEW_VERSION before bumping."
    exit 1
fi

# --- Update version files ---
echo ""
echo "Updating VERSION..."
echo "$NEW_VERSION" > VERSION

echo "Updating web/package.json..."
sed -i "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" web/package.json

# --- Verify both files were updated ---
echo ""
echo "Verifying changes..."
CHANGED_FILES=$(git diff --name-only)
for f in VERSION web/package.json; do
    if echo "$CHANGED_FILES" | grep -q "^$f$"; then
        echo "  OK: $f"
    else
        echo "  FAIL: $f was not modified!"
        echo "Aborting. Restoring files..."
        git checkout -- .
        exit 1
    fi
done

# --- Commit and tag ---
echo ""
echo "Committing..."
git add VERSION web/package.json
git commit -m "Release v$NEW_VERSION"

echo "Creating tag v$NEW_VERSION..."
git tag "v$NEW_VERSION"

echo ""
echo "Done! Version bumped to $NEW_VERSION"
echo ""
echo "To push the release:"
echo "  git push origin master --tags"

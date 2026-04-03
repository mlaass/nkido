Generate release notes and update CHANGELOG.md.

## Instructions

1. Find the last git tag:
   ```
   git describe --tags --abbrev=0
   ```

2. Get all commits since that tag:
   ```
   git log <last-tag>..HEAD --oneline
   ```
   If there are no commits since the last tag, report that there's nothing to release and stop.

3. Determine the version number:
   - If an argument was provided ("$ARGUMENTS"), use it as the version (strip leading "v" if present).
   - Otherwise, read the current version from the `VERSION` file at the repo root.

4. Get today's date in YYYY-MM-DD format.

5. Categorize each commit into sections based on its message:
   - **Added**: New features, functionality, or capabilities (commits starting with "Add", "Implement", "Introduce", "Support")
   - **Changed**: Modifications to existing behavior (commits starting with "Update", "Refactor", "Improve", "Rename", "Move", "Migrate")
   - **Fixed**: Bug fixes (commits starting with "Fix", "Resolve", "Correct", "Patch")
   - **Removed**: Removed functionality (commits starting with "Remove", "Delete", "Drop", "Deprecate")

   Rules:
   - Omit sections with no entries.
   - Clean up messages: remove verb prefixes and write as short descriptions starting with a capital letter.
   - Skip: merge commits, version bumps ("Release v..."), and PRD/documentation-only commits.
   - Group related commits (e.g., multiple commits fixing the same feature) into a single entry.

6. If `CHANGELOG.md` does not exist, create it with this header:
   ```markdown
   # Changelog

   All notable changes to this project will be documented in this file.

   The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
   and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
   ```

7. Insert the new version entry after the header (before the first `## [` entry if one exists). Use this format:

   ```markdown
   ## [X.Y.Z] - YYYY-MM-DD

   ### Added
   - Description

   ### Changed
   - Description

   ### Fixed
   - Description
   ```

8. Show a summary of what was added to the changelog.

Important: Do NOT commit the changes -- just update the file. I will review and commit manually.

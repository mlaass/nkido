---
name: update-changelog
description: Use when asked to update the CHANGELOG, prepare release notes, or write changelog entries for a new version of nkido. Drafts categorized entries from recent commits in Keep a Changelog format and writes them to CHANGELOG.md.
---

# Update Changelog

## Purpose

Draft a CHANGELOG.md section for an upcoming nkido release. The CHANGELOG is
the single source of truth for release notes — the GitHub Release body is
extracted from it on tag push by `.github/workflows/deploy.yml`, so the
quality of this entry directly determines what users see on GitHub.

## Conventions (do not break)

- Format: [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
  already established at the top of `CHANGELOG.md`.
- Categories (in this order, omit empty ones):
  `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`.
- Versioned heading format: `## [X.Y.Z] - YYYY-MM-DD`.
- Newest version on top, directly below the preamble.
- `VERSION` file is the semver source of truth. `web/package.json` is kept
  in sync by `scripts/bump-version.sh` — do not edit it here.

## Inputs you must gather first

Run these (in parallel when possible) before drafting anything:

1. `cat VERSION` — the current released version.
2. `git tag --list 'v*' | sort -V | tail -5` — recent tags.
3. `git log <last-tag>..HEAD --oneline` — commits since the last release.
4. `head -40 CHANGELOG.md` — confirms format and shows whether an
   `## [Unreleased]` or already-versioned section is in flight.

## Mode detection

After reading CHANGELOG.md, decide which mode you are in:

- **Accrual mode** — no in-progress section exists. Insert a new
  `## [Unreleased]` block above the most recent versioned heading and fill
  it with categorized entries derived from commits since the last tag.
- **Finalize mode** — an `## [Unreleased]` block exists and the user has
  asked to cut a release (e.g. "finalize 0.1.2", "release patch"). Compute
  the next version (`bump-version.sh` logic: major resets minor+patch,
  minor resets patch, patch increments) and rename `[Unreleased]` to
  `[X.Y.Z] - YYYY-MM-DD` using today's date. Add or refine entries from any
  commits the Unreleased section is missing.
- **Direct-versioned mode** — user explicitly asks for a specific version
  number (e.g. "write a 0.1.2 entry"). Skip Unreleased; insert
  `## [X.Y.Z] - YYYY-MM-DD` directly.

If unsure, ask the user which mode (one short question, then proceed).

## Drafting rules

- Read each commit's message and (if the subject is terse) `git show --stat`
  the commit to understand scope.
- Group commits by intent, not by author or chronology:
  - **Added**: new opcodes, builtins, UI components, public APIs, files.
  - **Changed**: rewrites/behavioral changes to existing features. UI
    redesigns belong here unless purely additive.
  - **Fixed**: bug fixes, crash fixes, off-by-ones, regressions.
  - **Removed**: deleted opcodes, retired flags, dropped browser support.
  - **Deprecated**: still works but slated for removal.
  - **Security**: vulnerabilities and CVE-class fixes.
- One bullet per user-visible change, not per commit. Squash several
  commits into one bullet when they describe one thing (e.g. "Add chorus
  effect" + "Tune chorus depth defaults" → single Added bullet).
- Skip pure-internal commits (refactors with no observable effect, comment
  fixes, CI tweaks, doc-only changes that do not change usage). When in
  doubt, omit — the changelog is for users, not contributors.
- Bullet voice: imperative or noun-phrase, matching the existing 0.1.0
  entries (e.g. "Hot-swap live coding with crossfade transitions").
- Reference user-facing concepts (opcode names, UI panels, language
  features) over file paths.

## Confirmation step

Before writing to CHANGELOG.md:

1. Show the user the proposed entries as a markdown block in chat.
2. Mention any commits you intentionally skipped, in one line each, so the
   user can object if you dropped something they consider user-visible.
3. Wait for approval (or edits) before saving.

## Writing to CHANGELOG.md

- Use the `Edit` tool, not `Write` — preserve the preamble and existing
  versioned sections exactly.
- Insert the new section directly under the preamble (above the previous
  newest version).
- Leave one blank line between sections.

## After saving

Tell the user the next steps in one short message:

```
Review:  git diff CHANGELOG.md
Bump:    ./scripts/bump-version.sh <major|minor|patch>
Push:    git push origin master --tags
```

The bump script will refuse to proceed if `## [X.Y.Z]` is missing, so the
order matters: changelog first, then bump, then push. CI will create the
GitHub Release from the changelog body automatically.

## Anti-patterns

- Do not edit `web/package.json` or `VERSION` — that is `bump-version.sh`'s
  job. Editing them here causes the bump script to abort on a dirty tree.
- Do not run `bump-version.sh`, `git tag`, or `git push` from the skill.
  The user runs those after reviewing the changelog diff.
- Do not invent commits or features. Every bullet must trace back to a
  real change in `git log <last-tag>..HEAD`.
- Do not rewrite or reorder existing versioned entries — only the new
  section is in scope.
- Do not add attribution lines, "co-authored-by" trailers, or commit SHAs
  to the changelog.

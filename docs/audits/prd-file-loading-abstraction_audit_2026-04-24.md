# Audit: Cross-Platform File Loading Abstraction

**PRD:** `docs/prd-file-loading-abstraction.md`
**Audit base:** `1b7f8278c33f9fbee72b459b45f536daf2a1055c` (2026-02-05) — PRD creation commit
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 3 of 5 primary goals fully met, 1 partial (format support), 1 explicit non-goal (streaming)
- Critical findings: 5 total — Unmet=3, Stubs=0, Coverage Gaps=1, Missing Tests=1, Scope Drift=0
- Recommended PRD Status: **Partially Done**
- One-line verdict: Core audio file-loading abstraction shipped and used by the web app, but SF2/SF3 pipeline (PRD §4.3, §4.5, §8 Phase 4, §9) is absent, and `load_mapped`/cache-stats UI are missing; PRD top-line banner `Status: DONE` (line 1) conflicts with `Status: Draft` (line 5) and overstates reality.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| Parsers take memory buffers, not paths | Met | `cedar/include/cedar/io/buffer.hpp:9-53` (MemoryView/OwnedBuffer); `cedar/include/cedar/audio/wav_loader.hpp:57` (load_from_memory(MemoryView)); `cedar/include/cedar/io/audio_decoder.hpp:28-41` |
| Platform-specific loaders | Met | Native `cedar/src/io/file_loader.cpp:9-53`; Web `web/src/lib/io/file-loader.ts:27-113` |
| Format support SF2/SF3/WAV/OGG/FLAC/MP3 | Partial | WAV/OGG/FLAC/MP3 in `cedar/src/io/audio_decoder.cpp:120-227` with vendored decoders `cedar/third_party/{stb_vorbis.c,dr_flac.h,minimp3.h}`. SF2/SF3 not wired through this abstraction. |
| Lazy loading | Met (web) | `web/src/lib/audio/bank-registry.ts:237`; `web/src/lib/stores/audio.svelte.ts:1186,1403` |
| Streaming for >50MB | Deferred | PRD §12 explicit non-goal for v1 |
| Zero virtual dispatch / async API / unified errors | Met | `cedar/include/cedar/io/errors.hpp:7-19` mirrors `web/src/lib/io/errors.ts:5-15` |

## Findings

### Unmet Goals
1. **SoundFont loader abstraction missing.** None of these (from §4.3, §4.5, §8 Phase 4, §9) exist: `cedar/include/cedar/io/soundfont_loader.hpp`, `cedar/src/io/soundfont_loader.cpp`, `web/src/lib/io/soundfont-loader.ts`, WASM export `cedar_load_soundfont_preset`, SerializedPreset/SerializedZone format. Pre-existing `cedar/src/audio/soundfont.cpp:84` uses tsf directly without MemoryView.
2. **`FileLoader::load_mapped` not implemented** (§4.2, §10.1). `cedar/include/cedar/io/file_loader.hpp:36-46` exposes only `load/exists/file_size`; no mmap/CreateFileMapping anywhere outside third_party.
3. **TS cache management surface incomplete.** PRD §4.4 specifies `isCached`, `clearCache`, `getCacheStats`; only `get/set/delete/clear` exist in `web/src/lib/io/file-cache.ts:47-133`. No cache-management UI (§8 Phase 2 item 4).

### Stubs
None. `audio_decoder.cpp`, `file_loader.cpp`, `file-cache.ts`, `file-loader.ts` are complete.

### Coverage Gaps
- No tests under `web/src/lib/io/`. IndexedDB LRU eviction (`file-cache.ts:135-172`) is untested.

### Missing Tests
- C++ coverage fine: `cedar/tests/test_file_loader.cpp` (22 TEST_CASE/SECTION, 210 LOC), `cedar/tests/test_audio_decoder.cpp` (19 entries, 218 LOC).
- No TS unit tests; no integration test for `cedar_load_audio_data` + `loadSampleAudio` worklet round-trip (`web/static/worklet/cedar-processor.js:168,890`).

### Scope Drift
None in implemented portions. Follow-up commit `44cc862` deleted TS `audio-decoder.ts`/`format-detect.ts` and moved OGG/FLAC/MP3 decoding entirely to C++ — a reasonable refinement of §2.2's hybrid approach, not drift.

### Suggestions
- Reconcile PRD line 1 (`Status: DONE`) vs line 5 (`Status: Draft`) and the actual state.
- Either implement SoundFont loader or cleanly split that scope into `prd-soundfonts-sample-banks.md` (already referenced at PRD line 8).
- Add `load_mapped` if the "100MB SF2 in <2s" success metric (§13) is still targeted.
- Add TS tests for `FileCache.evictIfNeeded` LRU boundaries and `cacheKeyFromFile`.
- Expose `getCacheStats` and a cache-management UI per §4.4/§8.4.

## PRD Status
- Current: `Draft` (line 5), with conflicting `Status: DONE` banner (line 1) referencing commit a3283d3
- Recommended: `Partially Done` — or split SF2/SF3 out and mark remainder `Done`
- Reason: Core buffer + file-loader + audio-decoder abstraction is implemented, used, and tested on the C++ side; SoundFont handling, `load_mapped`, and TS cache-management surface are absent.

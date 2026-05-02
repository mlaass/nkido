# File-Loading Semantics Review

**Date:** 2026-05-02
**Author:** Claude (investigation)
**Status:** Assessment — no code changes proposed in this document
**Related PRDs:** prd-file-loading-abstraction, prd-soundfonts-sample-banks,
prd-smooch-wavetable-synth, prd-sample-loading-before-playback,
prd-audio-input, prd-soundfont-playback-fixes

## 1. Context and Scope

File loading has accreted across five PRDs that landed in sequence. The first
(`prd-file-loading-abstraction`, commit a3283d3) introduced a unified
abstraction. Subsequent PRDs each added their own ingestion path: SoundFonts,
wavetable banks, sample banks with manifests, live audio input from files,
and a fix-up pass for SoundFont preload races. Each shipped working code,
but no one has stepped back to look at the layers as a single surface.

The user asked for an honest survey: where are we redundant, where are we
inconsistent, is a refactor warranted, and is HTTP-from-akkado
(Strudel-style URLs in user code) worth a follow-up PRD. The user also
proposed framing the abstraction as **pluggable protocol handlers** — a
single resolver that knows `file://`, `https://`, and could be extended with
`res://` for a Godot host or `idb://` for IndexedDB. That framing turns out
to be the most useful lens; it shows up in §7.

This is investigation only. Implementation and new PRDs come later.

## 2. Inventory: Subsystems and Entry Points

### 2.1 C++ side

| Subsystem | Header | Public load API | Uses io/ abstraction? |
|---|---|---|---|
| Generic file I/O | `cedar/include/cedar/io/file_loader.hpp` | `FileLoader::load(path)` returns `LoadResult` (variant of `OwnedBuffer` / `FileLoadError`) | yes (the source of it) |
| Generic memory views | `cedar/include/cedar/io/buffer.hpp` | `MemoryView`, `OwnedBuffer` | yes |
| Multi-format audio decoder | `cedar/include/cedar/io/audio_decoder.hpp` | `AudioDecoder::decode(MemoryView)` → `DecodedAudio` | yes |
| WAV parser | `cedar/include/cedar/audio/wav_loader.hpp` | `WavLoader::load_from_file(path)` and `load_from_memory(MemoryView)` and `load_from_memory(uint8_t*, size_t)` | partially — defines and consumes `MemoryView` |
| SoundFont registry | `cedar/include/cedar/audio/soundfont.hpp:145` | `SoundFontRegistry::load_from_memory(const void*, int, name, SampleBank&)` | **no** — raw `void*`/`int` |
| Wavetable registry | `cedar/include/cedar/wavetable/registry.hpp:51,55` | `load_from_file(name, path, ...)` and `load_from_memory(name, uint8_t*, size_t, ...)` | partially — uses WAV loader, not `MemoryView` |
| Sample bank | `cedar/include/cedar/vm/sample_bank.hpp:88,129,144,162` | `load_sample(name, float*, ...)`, `load_wav_file`, `load_wav_memory`, `load_audio_data(name, MemoryView)` | partially |

### 2.2 Web/TypeScript side

| Subsystem | File | Public load API |
|---|---|---|
| Generic file loader | `web/src/lib/io/file-loader.ts` (153 lines) | `loadFile(source, options)` accepting `{type:'url'\|'file'\|'arraybuffer', ...}` |
| IndexedDB cache | `web/src/lib/io/file-cache.ts` (186 lines) | `FileCache` singleton with LRU eviction at 500MB |
| Errors | `web/src/lib/io/errors.ts` (15 lines) | `FileLoadError` class |
| Sample-bank registry | `web/src/lib/audio/bank-registry.ts` (355 lines) | `loadBank(source, name?)`, `loadFromGitHub(repo)`, `resolveSample(ref)` |
| Audio store | `web/src/lib/stores/audio.svelte.ts` | `loadSampleFromUrl`, `loadSoundFontFromUrl`, `loadWavetableFromUrl` (lines ~1313, 1511, 1562) |

### 2.3 WASM bridge (`web/wasm/nkido_wasm.cpp`)

| Export | Line | Signature | Path it calls |
|---|---|---|---|
| `cedar_load_sample` | 320 | `(name, float* audio_data, num_samples, channels, sample_rate)` | `vm.load_sample` (raw f32 already decoded) |
| `cedar_load_sample_wav` | 339 | `(name, uint8_t* wav_data, size)` | `sample_bank.load_wav_memory` |
| `cedar_load_audio_data` | 357 | `(name, uint8_t* data, size)` | `sample_bank.load_audio_data(MemoryView)` (auto-detect) |
| `cedar_load_wavetable_wav` | 419 | `(name, uint8_t* wav_data, size)` | `wavetable_registry.load_from_memory` |
| `cedar_load_soundfont` | 1855 | `(uint8_t* data, int size, name)` ← name **last** | `soundfont_registry.load_from_memory` |

Five entry points. Three different parameter orderings. Two of them
(`cedar_load_sample_wav`, `cedar_load_audio_data`) overlap on inputs; the
former is a strict subset since `load_audio_data` auto-detects the format.

## 3. Implementation vs PRD Divergence

The original PRD (`prd-file-loading-abstraction.md`) is marked DONE. Most of
the core landed; some items shipped differently or not at all.

### Shipped as designed
- `MemoryView`/`OwnedBuffer` types (`cedar/include/cedar/io/buffer.hpp`)
- Native `FileLoader::load(path)` and `exists`/`file_size` helpers
- Multi-format `AudioDecoder` (WAV, OGG, FLAC, MP3) with vendored stb_vorbis,
  dr_flac, minimp3
- Error types (`FileError` enum, `FileLoadError` struct)
- TS `loadFile()` with URL/File/ArrayBuffer sources, progress, abort, cache
- IndexedDB cache with 500MB LRU eviction

### Skipped or shipped differently
- **`FileLoader::load_mapped()`** never landed. The header at
  `cedar/include/cedar/io/file_loader.hpp` exposes only `load`, `exists`,
  `file_size`. Verify with `grep load_mapped cedar/include/cedar/io/`.
  Probably fine — no current asset is large enough for mmap to matter, but
  the PRD §10.1 promised it.
- **`SerializedZone` / `cedar_load_soundfont_preset()`** never landed.
  PRD §4.5 designed a 40-byte-per-zone wire format for shipping parsed SF2
  data from TS to C++; the actual `cedar_load_soundfont` (line 1855) takes
  raw SF2 bytes and parses them in C++ via TinySoundFont. Pragmatic for now,
  but means the WASM heap holds the full SF2 during parse.
- **`decodeAudioFile()` TS helper** never landed. PRD §4.4 specified using
  Web Audio's `decodeAudioData()` for OGG/FLAC/MP3 in the browser; actual
  code passes raw bytes to WASM and uses C++ `AudioDecoder` for all
  formats. The deferral is recorded as an inline comment at
  `web/src/lib/stores/audio.svelte.ts:1313` ("C++/WASM decodes all
  formats"). One codebase for decode is good; the trade-off is WASM heap
  pressure when a single sample exceeds tens of MB.
- **No `SoundFontLoader` under `cedar/include/cedar/io/`.** PRD §9 listed
  `soundfont_loader.hpp` as new. SF2 lives at
  `cedar/include/cedar/audio/soundfont.hpp` and bypasses the io/
  abstraction entirely (see §4.2 below).
- **Format byte parameter** on `cedar_load_audio_data`. PRD §4.3 specified
  an `uint8_t format` argument; actual signature drops it for magic-byte
  sniffing inside `AudioDecoder::detect_format`. Simpler API; fine.
- **No cache-management UI.** PRD §6 mentioned one. Cache is functional but
  invisible to users.

The audit at `docs/audits/prd-file-loading-abstraction_audit_2026-04-24.md`
already flagged the missing `load_mapped`, missing SoundFont loader, and
missing UI. That audit is consistent with what's still missing today.

## 4. Redundancies

### 4.1 Five WASM load entry points
File: `web/wasm/nkido_wasm.cpp` lines 320, 339, 357, 419, 1855.

`cedar_load_sample_wav` (line 339) is dead-weight overlap with
`cedar_load_audio_data` (line 357). The latter handles WAV plus OGG/FLAC/MP3
via auto-detection. The worklet uses both: `cedar-processor.js:930` for the
already-decoded float path via `_cedar_load_sample`, and `:979` for the
generic byte path via `_cedar_load_audio_data`. There is no caller for
`_cedar_load_sample_wav` in the worklet I could find (verify with
`grep -n cedar_load_sample_wav web/static/worklet/`).

`cedar_load_sample` (raw float) is also borderline — the worklet only uses
it for one specialized path, and could in principle be replaced if the
audio store handed bytes instead of pre-decoded floats. But it is a real
performance shortcut for cases where TS already has a `Float32Array`.

### 4.2 SoundFont uses `void*` instead of `MemoryView`
File: `cedar/include/cedar/audio/soundfont.hpp:145`.

```cpp
int load_from_memory(const void* data, int size, ...);
```

Compare to the wavetable registry at `cedar/include/cedar/wavetable/registry.hpp:55`:
```cpp
int load_from_memory(name, const std::uint8_t* data, std::size_t size, ...);
```

And `WavLoader::load_from_memory(MemoryView)` at
`cedar/include/cedar/audio/wav_loader.hpp:57`.

Three different API shapes for the same conceptual operation. The
SoundFont signature also takes `int size`, which is signed and 32-bit on
most platforms — not a real problem for SF2 sizes today but inconsistent
with `std::size_t` everywhere else.

### 4.3 Four ingestion paths into `SampleBank`
File: `cedar/include/cedar/vm/sample_bank.hpp` lines 88, 129, 144, 162.

```cpp
load_sample(name, const float* data, num_frames, channels, sample_rate);
load_wav_file(name, filepath);
load_wav_memory(name, const uint8_t* data, size);
#ifndef CEDAR_NO_AUDIO_DECODERS
load_audio_data(name, MemoryView data);  // multi-format, magic-byte detect
#endif
```

The first three pre-date the abstraction; the fourth is what landed with
the abstraction. The first is genuinely useful (already-decoded PCM); the
middle two are now redundant with the fourth (which handles WAV via
`AudioDecoder` → `WavLoader::load_from_memory`). Collapsing to two paths
(`load_sample(float*)` for already-decoded and `load_audio_data(MemoryView)`
for everything else) would simplify both the C++ surface and the WASM
bridge.

### 4.4 WAV is the only format with a file-path API
WAV has both `load_from_file(path)` and `load_from_memory(MemoryView)`.
OGG, FLAC, MP3 are reachable only via `AudioDecoder::decode(MemoryView)` —
to load one of them from a file path on native, the caller has to do:

```cpp
auto result = FileLoader::load(path);
if (!result.success()) return error;
auto decoded = AudioDecoder::decode(result.buffer().view());
```

Not terrible, but means `load_from_file` exists asymmetrically for WAV.
The CLI tools currently only load WAV from path, so this hasn't bitten,
but it's a footgun for anyone reaching for OGG sample banks on native.

### 4.5 GitHub manifest path bypasses (and double-fetches) FileLoader
File: `web/src/lib/audio/bank-registry.ts:170`.

```ts
const response = await fetch(url);            // line 170 — raw fetch, not loadFile
// ... parse manifestData, override _base ...
return this._loadBankInternal(url, bankName); // line 183 — calls loadFile internally
```

`_loadBankInternal` at line 89 calls `loadFile({type:'url', url}, {cache:true})`
on the **same URL**. The first fetch is purely to read `_name` from the
manifest, but the override of `manifestData._base` on line 178 is then
discarded — `_loadBankInternal` re-fetches and re-parses, and that copy
doesn't have the override. The fallback path in `_loadBankInternal` at
lines 99–106 derives `_base` from the URL anyway, so the GitHub path works,
but the first fetch is dead network traffic.

This is two bugs in one: (a) raw `fetch()` bypasses the IndexedDB cache for
the first hit, and (b) the URL is fetched twice on every cold load.

### 4.6 SoundFont registry has no thread guard, others do
`WavetableBankRegistry` uses `std::mutex` plus a snapshot pattern for
audio-thread reads (see `cedar/include/cedar/wavetable/registry.hpp` and
the snapshot members). `SoundFontRegistry` has no visible mutex at the
public API. Currently mitigated in practice by the name-based dedup added
in `prd-soundfont-playback-fixes` (commit history) which prevents
double-loads from the compile/preload race. The asymmetry is real even
if benign.

### 4.7 No first-class file-loading builtin in akkado
Akkado source can't reference an asset by URI. The compiler emits
`required_samples` / `required_soundfonts` / `required_wavetables` as part
of `CompileResult` and the host (CLI or web) is responsible for resolving
those names. There's no `samples("https://...")` or
`bank("github:tidalcycles/...")` expression.

This isn't a redundancy with anything else, but it's the precise gap that
a Strudel-style HTTP feature would fill — see §7 and §8.

### 4.8 PRD-specified format byte never landed, magic-byte sniffing instead
Already noted in §3. Listed here because it's a place where the WASM
bridge intentionally diverged from the PRD, not a mistake.

## 5. Inconsistencies (Not Strictly Redundant, but Surface-Level Drift)

### 5.1 Parameter ordering on WASM exports
- `cedar_load_audio_data(name, data, size)` — name first
- `cedar_load_sample_wav(name, data, size)` — name first
- `cedar_load_wavetable_wav(name, data, size)` — name first
- `cedar_load_soundfont(data, size, name)` — name **last**

Probably an accident of when each export was added. Easy to align if we
rev the WASM API.

### 5.2 Return-type semantics
- Sample loaders return `uint32_t` with `0` as failure
- `cedar_load_soundfont` returns `int` with `-1` as failure
- `cedar_load_wavetable_wav` returns `int32_t` with `-1` as failure

Three types, two failure conventions. Each works in isolation; mixing them
makes the worklet handler code more verbose than it needs to be (see
`web/static/worklet/cedar-processor.js` around lines 998 and 1078).

### 5.3 Naming
- `WavLoader::load_from_file` / `load_from_memory`
- `WavetableBankRegistry::load_from_file` / `load_from_memory`
- `SoundFontRegistry::load_from_memory` (no file variant)
- `SampleBank::load_wav_file` / `load_wav_memory` / `load_sample` / `load_audio_data`
- TS `loadFile` (camelCase entry point that handles all sources)

There is no canonical verb. "load_from_X" dominates on the C++ side,
"loadX" on the TS side, but `SampleBank` breaks the pattern and uses
"load_X" instead.

### 5.4 What "load" means semantically
- For `WavLoader`, "load" means "parse and return PCM"
- For `SampleBank`, "load" means "parse and register, return ID"
- For `SoundFontRegistry`, "load" means "parse, expand sample data into
  another registry (the SampleBank), and register, return ID"
- For `loadFile` in TS, "load" means "fetch bytes and return ArrayBuffer"

These are all reasonable, but they form a layering that isn't documented
anywhere. A new contributor has to read several headers to figure out
which "load" they want.

## 6. The Refactor Case (Targeted)

There are three clusters worth consolidating, ordered by leverage. The
biggest one is §7 (the protocol-handler abstraction); the two below are
local cleanups that should land first or alongside it.

### 6.1 WASM surface collapse — small, mechanical, low risk
- Deprecate `cedar_load_sample_wav` (covered by `cedar_load_audio_data`).
- Reorder `cedar_load_soundfont` parameters to match the rest:
  `(name, data, size)`.
- Pick one return convention (recommend `int32_t` with `-1` failure since
  it's already the convention on the newer exports).
- Optional: introduce a unified `cedar_load_asset(kind, name, data, size)`
  with a `kind` enum (sample / soundfont / wavetable). Probably not worth
  it — the per-kind paths will exist on the C++ side regardless.

Effort: half a day. Touches ~5 functions in `nkido_wasm.cpp` and
corresponding worklet handlers. Backward compat is straightforward — keep
the old exports for one rev with `[[deprecated]]` annotations or simply
remove since the worklet is the only consumer.

### 6.2 Push `MemoryView` through the C++ registries
- Change `SoundFontRegistry::load_from_memory` from `(void*, int)` to
  `(MemoryView)` plus name/SampleBank& (or whatever ergonomics suggest).
- Add an overload to `WavetableBankRegistry::load_from_memory` taking
  `MemoryView`.
- Collapse `SampleBank` to two ingestion paths:
  - `load_pcm(name, const float*, num_frames, channels, sample_rate)` —
    already-decoded shortcut
  - `load_audio_data(name, MemoryView)` — handles all encoded formats
  (Mark the existing `load_wav_file`, `load_wav_memory` as deprecated,
  remove them in a follow-up. Ideally remove now since the only callers
  are the WASM bridge functions also being deprecated in 6.1.)

Effort: one to two days. Mostly mechanical, some tests need updating.

### 6.3 Fix the bank-registry GitHub double-fetch
File: `web/src/lib/audio/bank-registry.ts:154–185`.

Replace the raw `fetch(url)` at line 170 with `loadFile({type:'url', url}, {cache:true})`,
or better, drop the first fetch entirely and let `_loadBankInternal` handle
both fetch and parse (it already extracts `_name` correctly).

Effort: 30 minutes. Net win: one fewer network round-trip on every cold
GitHub bank load, plus the first hit gets cached.

### 6.4 What we should NOT do
- **Don't ship mmap support** (`load_mapped`) until a workload demands it.
  Default soundfonts are <40MB; samples and wavetables are smaller. Adding
  platform-specific mmap code for a hypothetical workload is busywork.
- **Don't ship the `SerializedZone` format.** The current "send bytes to
  WASM, parse with TinySoundFont" path works. The optimization saves heap
  but adds a serialization protocol that has to evolve in lockstep across
  TS and C++. The gain doesn't justify the friction.
- **Don't add a Web Audio decoder shim** in TS. C++ `AudioDecoder` is the
  decoder of record; keep it that way. The WASM heap pressure for large
  samples is real but predictable.

## 7. Protocol-Handler Abstraction (The Strongest Reframing)

The user proposed framing file loading as **pluggable protocol handlers**
keyed on URI scheme. This frame turns several of the current paint points
into one design.

### 7.1 Current state, viewed by scheme
- `https://`/`http://` is understood by `loadFile({type:'url', url})` on the
  web; native has no HTTP support at all.
- `github:user/repo` is understood by `BankRegistry.loadFromGitHub`, which
  hand-rolls URL construction and fetches separately.
- A bare filesystem path is understood by native `FileLoader::load(path)`;
  WASM's `FileLoader` is a no-op stub guarded by `__EMSCRIPTEN__`.
- IndexedDB is consulted as a transparent cache layer, not as an
  addressable backing store. There is no `idb://key` or equivalent.
- A future Godot host would need `res://path` for Godot's
  `ResourceLoader`. Today, that integration would have to be bolted on
  with a parallel API.

Five schemes, four code paths, no shared resolver. Adding a new host
(Godot, Tauri desktop, Bun CLI) means writing the same fetch-a-URI logic
again.

### 7.2 What a resolver would look like
A single `FileResolver` interface with handlers registered per scheme,
returning bytes to the caller in a uniform shape. Sketch only — actual
design would be a separate PRD:

```
file://abs/path           → ifstream (native), error (wasm)
http(s)://...             → fetch (web), libcurl/cpp-httplib (native), HTTPRequest (godot)
res://path                → Godot ResourceLoader (godot only)
github:user/repo[/path]   → resolves to https:// internally and recurses
idb://key                 → IndexedDB direct read (web only)
bundled://name            → embedded asset (any env)
```

Per-scheme handlers expose `canHandle(uri)` and `load(uri, options)`. The
return type stays `LoadResult` / `Promise<LoadResult>` over `OwnedBuffer`.
Decoding is unchanged: `AudioDecoder::decode(buffer.view())` keeps doing
its job. The resolver only owns "where the bytes come from".

A bare path with no scheme defaults to `file://` for back-compat, so
existing native callers don't change.

### 7.3 What this consolidates
- `loadFile({type:'url', url})` and `BankRegistry.loadFromGitHub` collapse
  into one entry point: `loadFile('github:user/repo')` recursively resolves
  to `loadFile('https://raw.githubusercontent.com/...')`.
- The double-fetch in §4.5 becomes impossible to write — there is one
  fetch path.
- IndexedDB caching becomes a transparent middleware applied to any
  scheme that opts in, not a special case for URLs.
- Native `nkido-cli` gains HTTP support for free as soon as someone
  registers an HTTP handler (libcurl or cpp-httplib).
- Godot porting is "register `res://` and `https://` handlers, leave the
  rest alone".

### 7.4 Trade-offs
**Pros:**
- Single porting surface across native / WASM / Godot.
- Clean separation: "where" (resolver) vs "what" (decoder) vs "register"
  (sample bank / soundfont / wavetable).
- Strudel-style HTTP from akkado becomes a near-trivial follow-up — see
  §8.
- Caching becomes uniform middleware, not per-call-site `cache: true`.

**Cons:**
- Non-trivial refactor of every existing call site. The TS surface has to
  change from the discriminated-union `FileSource` to a URI string.
- API design has to span three ABIs (C++, WASM bridge, TS). URI parsing
  has corner cases (encoding, query strings, auth headers). The work is
  not as small as 6.1 or 6.2.
- Risk of scope creep: if the resolver grows to support directory listing,
  partial reads, writes, or watch-for-changes, it becomes a generic VFS
  and the simplicity argument evaporates. The PRD will need to scope
  hard: "fetch bytes by URI, nothing else."

### 7.5 Recommendation
**Yes, with bounded scope.** This is the single biggest streamlining win
in the system. The targeted cleanups in §6.1 and §6.2 should land first or
in the same series so the resolver lands on top of clean primitives, not
on top of three different `load_from_memory` shapes.

A separate PRD (`prd-file-protocol-resolver.md` or similar) should cover:
the resolver interface, registration model, scheme registry, default
schemes per platform, caching middleware, error mapping, and the
deprecation path for the existing `loadFile` source-union.

## 8. Strudel-Style HTTP Loading from Akkado

### 8.1 What already exists
- `BankRegistry` parses Strudel-compatible `strudel.json` manifests
  (sample-name → variant URL array, with `_base` URL prefix).
- `loadFromGitHub('github:user/repo')` resolves to
  `https://raw.githubusercontent.com/...` and registers the bank.
- `loadBank(url, name?)` accepts a raw HTTPS URL today.
- Sample variants are lazy-loaded on first reference via
  `resolveSample(ref)`, which delegates to
  `audioEngine.loadSampleFromUrl(qualifiedName, fullUrl)`.
- IndexedDB cache is engaged by default for variant downloads.

In other words: the **runtime** already supports Strudel-style HTTP.
The piece that's missing is exposing it through akkado source.

### 8.2 What's missing
Akkado source can't reference an HTTP URL. There's no:
```akkado
samples("https://example.com/tr808/strudel.json")
samples("github:tidalcycles/Dirt-Samples")
s("bd").bank("https://...")
```

The host has to call `bankRegistry.loadBank(url)` out-of-band before
compile, or rely on bundled banks registered at startup.

### 8.3 Cleanest path
Ride on the protocol-handler abstraction (§7). Once a resolver exists,
adding akkado syntax is mechanical:

1. Compiler-time builtin `samples(uri)` (or `bank(uri)`) records the URI
   in `CompileResult` alongside the existing `required_samples` metadata.
2. The host (web or CLI) reads that list and calls `resolver.load(uri)`
   for each, then `bankRegistry.loadFromArrayBuffer` (or equivalent) to
   register.
3. The existing pre-play barrier (`prd-sample-loading-before-playback`)
   already ensures playback waits for samples to finish loading.

### 8.4 What a follow-up PRD must define
This is for a **second** PRD, after the resolver lands:
- Akkado syntax: `samples(uri)`, `bank(uri)`, or fluent on patterns
  (`s("bd").bank(uri)`). Pick one, document interaction with the existing
  `bank()` modifier from `prd-soundfonts-sample-banks`.
- Compile-time vs runtime resolution. Compile-time is much safer:
  `CompileResult` lists URIs to be fetched, host fetches before flipping
  the audio thread to the new bytecode. Runtime fetches in the audio path
  are out of the question (RT-unsafe).
- CLI parity. Native `nkido-cli` needs an HTTP handler registered. Likely
  cpp-httplib (header-only, no extra deps) or libcurl.
- Security. Default-deny on native (CLI flag to opt in, allow-list of
  domains). Web is already domain-isolated by the browser. Max sizes and
  timeouts both sides.
- Caching policy. Should an HTTP-bearing program cache the manifest
  permanently or revalidate (ETag / Last-Modified)? Web cache today is
  permanent up to 500MB LRU; revalidation is probably a nice-to-have, not
  a blocker.

### 8.5 Out of scope here
This report doesn't propose syntax or design HTTP semantics. It only
flags that:
- The web runtime already supports it; only akkado syntax is missing.
- A protocol-resolver PRD should land first so the akkado feature ships
  on top of clean abstractions.

## 9. Recommendation Summary

Ordered by leverage. Each is its own follow-up; this report does none of
them.

1. **Protocol-handler resolver** (`prd-file-protocol-resolver.md`).
   Biggest win. Subsumes the redundancy in §4.5, unblocks Strudel-from-
   akkado, unblocks Godot porting, makes caching uniform middleware. Scope
   carefully — "fetch bytes by URI" only, no VFS.

2. **Targeted cleanups** (could be one PRD or several smaller commits):
   - §6.1: Collapse the WASM surface — deprecate `cedar_load_sample_wav`,
     align `cedar_load_soundfont` parameter order and return type.
   - §6.2: Push `MemoryView` through `SoundFontRegistry` and
     `WavetableBankRegistry`; collapse `SampleBank` to two ingestion paths.
   - §6.3: Fix the bank-registry GitHub double-fetch.
   These can land before, after, or alongside (1). They're low-risk and
   self-contained.

3. **Akkado HTTP builtin** (`prd-akkado-http-samples.md` or similar).
   Land after (1). Adds one or two compiler builtins, plumbs URIs through
   `CompileResult`. The runtime work is ~zero on the web (already
   present) and modest on the CLI (register an HTTP handler).

### What we should not do
- mmap support for `FileLoader` — no workload demands it.
- `SerializedZone` wire format — current "ship full SF2 bytes" works.
- Web Audio decoder shim in TS — C++ `AudioDecoder` is the decoder of
  record.

## 10. Verification

Every claim in §3–§5 carries a file:line reference. To independently
confirm the absences:

```bash
# §3: load_mapped never landed
grep -rn "load_mapped" cedar/include/cedar/io/

# §3: no SoundFontLoader under io/
ls cedar/include/cedar/io/

# §3: no audio-decoder.ts or soundfont-loader.ts under web/src/lib/io/
ls web/src/lib/io/

# §4.1: cedar_load_sample_wav callers
grep -rn "cedar_load_sample_wav\|_cedar_load_sample_wav" web/static/worklet/ web/src/

# §4.5: GitHub double-fetch
sed -n '154,185p' web/src/lib/audio/bank-registry.ts

# §5.1: parameter orderings
grep -n "WASM_EXPORT.*cedar_load" web/wasm/nkido_wasm.cpp
```

No tests, no builds — this is a documentation deliverable.

## 11. References

- `docs/prd-file-loading-abstraction.md` — primary PRD (DONE)
- `docs/prd-soundfonts-sample-banks.md` — BankRegistry, SoundFont opcode
- `docs/prd-smooch-wavetable-synth.md` — wavetable preprocessor
- `docs/prd-sample-loading-before-playback.md` — pre-play barrier
- `docs/prd-soundfont-playback-fixes.md` — name-based dedup fix
- `docs/prd-audio-input.md` — file source for live input
- `docs/audits/prd-file-loading-abstraction_audit_2026-04-24.md`
- `docs/audits/prd-soundfonts-sample-banks_audit_2026-04-24.md`
- `docs/audits/prd-smooch-wavetable-synth_audit_2026-04-30.md`

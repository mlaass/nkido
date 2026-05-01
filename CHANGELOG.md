# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - 2026-05-01

### Added
- Smooch wavetable oscillator (`OSC_WAVETABLE`) with multi-bank support and band-limited mipmap tables
- Userspace state primitives: `state`, `get`, `set` cells backed by a new `STATE_OP` opcode for stateful patches in Akkado source
- Edge-triggered operators via the new `EDGE_OP` (replaces `SAH`) with mode-dispatched edge primitives
- UFCS method-call syntax: `x.foo(y)` falls through to `foo(x, y)`
- Pattern event arrays with typed prefixes and auto-coercion of patterns to scalar values
- Pattern transforms: `swing`, `swingBy`, `ply`, `linger`, `zoom`, `segment`, `early`, `late`, `palindrome`, `compress`, `iter`, `iterBack`
- Pattern generators: `run`, `binary`, `binaryN`
- Mini-notation record suffix for per-event fields, e.g. `c4{vel:0.8, dur:0.5}`
- Custom-property accessor with `bend`, `aftertouch`, `dur` transforms
- Voicing system: `anchor`, `mode`, `voicing`, `addVoicings` builtins
- Live audio input: `in()` builtin + `INPUT` opcode wired through the host
- Optional `step` parameter on `range()`; extended optional params across array utility builtins
- `nkido-cli render` mode for offline rendering, plus a Python polyphony experiment harness
- `/embed` route with a patches system and 10 landing-page demo patches
- `web-v*` tag pattern for web-only production deploys
- Hippocratic Code of Conduct

### Changed
- `poly()` signature reordered with a higher voice ceiling; debugger panel and F1 docs polished
- Renamed `fold` → `reduce` in the arrays reference; rebuilt arrays test coverage
- Smooch wavetable position smoothed at audio rate to suppress UI-cadence sidebands
- Hot-swap reliability: Ctrl+Enter is guaranteed to refresh audio even when block topology changes

### Removed
- `product` array builtin (replaced by `reduce`)

### Fixed
- `phaser` dry+wet summing; stages and feedback are now exposed as parameters
- Audio input panel showed "Audio not initialized" before pressing play
- Nested polyrhythms dropped voices; sample polyrhythms (`[bd, hh]`) now play both samples simultaneously
- `poly()` cycle-alignment at rational BPMs and required-input metadata
- `stepper-demo` array-through-closure binding with `STATE_OP`-gated writes
- Velocity-shorthand propagation in mini-notation
- Step-highlighting froze on the edited line and stayed frozen until recompile

## [0.2.0] - 2026-04-23

### Added
- Stereo signal semantics: automatic mono→stereo lifting, `mono()` downmix, stereo-aware builtin catalog with STEREO_INPUT flag
- `bpm` and `sr` builtin variables (desugar through ENV_GET, no new opcodes)
- `>>` and `@` as aliases for `|>` (pipe) and `%` (hole)
- Underscore placeholder (`_`) for skipping positional arguments to use their defaults
- Size-optimized Cedar build configuration and ESP32/Xtensa cross-compile support
- `cedar-size-report.sh` for tracking binary size across feature configurations
- `gm_medium` (FluidR3Mono, 14MB) and `gm_large` (MuseScore, 39MB) soundfont tiers

### Changed
- Rebranded web UI with orange accent and neutral grey palette
- Renamed project from enkido to nkido (canonical name going forward)
- Default soundfont switched from MuseScore_General (39MB) to TimGM6mb (2.6MB SF3)

### Fixed
- Windows and macOS portability blockers across build and runtime
- WASM build failure from missing project version in Cedar compile definitions

## [0.1.1] - 2026-04-03

### Fixed
- Pattern highlight corruption during edits by tracking document offset changes

## [0.1.0] - 2026-04-02

### Added
- Cedar audio synthesis engine with stack-based bytecode VM
- Akkado DSL compiler with Pratt parser and mini-notation support
- SvelteKit web application with CodeMirror editor
- WASM build for browser-based audio synthesis
- Hot-swap live coding with crossfade transitions
- 95+ DSP opcodes including oscillators, filters, delays, and reverbs
- Pattern sequencing with Strudel/Tidal-compatible mini-notation
- Runtime parameter controls (sliders, toggles, buttons, dropdowns)
- Theme system with 7 preset themes and custom theme support
- CI/CD pipeline with Netlify deployment

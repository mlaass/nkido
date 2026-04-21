# Cedar Binary Size Report

_Generated: 2026-04-21 10:14 UTC | Host: x86_64 | Commit: 84d861e_
_Compiler: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0_

## Size Summary

| Config | Description | Archive | Stripped | Text (sum) |
|--------|-------------|--------:|---------:|-----------:|
| `release` | Release, all features, -O3 | 701.3 KB | 484.6 KB | 468.1 KB |
| `minsize` | MinSizeRel, all features, -Os | 745.9 KB | 389.6 KB | 248.9 KB |
| `minsize-stripped` | MinSizeRel, no optional modules | 324.1 KB | 146.7 KB | 72.1 KB |
| `esp32` | ESP32 profile: stripped + reduced memory + float-only | 323.9 KB | 146.6 KB | 72.1 KB |

## Size Delta vs. `release`

| Config | Stripped | Δ bytes | Δ % |
|--------|---------:|--------:|----:|
| `release` | 484.6 KB | +0 | +0.0% |
| `minsize` | 389.6 KB | -97264 | -19.6% |
| `minsize-stripped` | 146.7 KB | -345998 | -69.7% |
| `esp32` | 146.6 KB | -346134 | -69.8% |

## Feature Matrix

### Toggles

| Feature | Default | `release` | `minsize` | `minsize-stripped` | `esp32` |
|---------|---------|------|------|------|------|
| `AUDIO_DECODERS` | ON | ✓ | ✓ | **✗** | **✗** |
| `SOUNDFONT` | ON | ✓ | ✓ | **✗** | **✗** |
| `FFT` | ON | ✓ | ✓ | **✗** | **✗** |
| `FILE_IO` | ON | ✓ | ✓ | **✗** | **✗** |
| `MINBLEP` | ON | ✓ | ✓ | **✗** | **✗** |
| `FLOAT_ONLY` | OFF | ✗ | ✗ | ✗ | **✓** |

### Memory

| Feature | Default | `release` | `minsize` | `minsize-stripped` | `esp32` |
|---------|---------|------|------|------|------|
| `BLOCK_SIZE` | 128 | 128 | 128 | 128 | 128 |
| `MAX_BUFFERS` | 256 | 256 | 256 | 256 | **64** |
| `MAX_STATES` | 512 | 512 | 512 | 512 | **128** |
| `MAX_VARS` | 4096 | 4096 | 4096 | 4096 | **512** |
| `MAX_PROGRAM_SIZE` | 4096 | 4096 | 4096 | 4096 | **1024** |
| `ARENA_SIZE` | 33554432 | 33554432 | 33554432 | 33554432 | **262144** |

_Bold = overridden from default. ✓/✗ = ON/OFF toggle._

## Per-object Text Sections

### `release`

| Object | Text | Data | BSS |
|--------|-----:|-----:|----:|
| `audio_decoder` | 233.9 KB | 0.1 KB | 1.0 KB |
| `vm` | 154.9 KB | 0.1 KB | 0.0 KB |
| `soundfont` | 53.2 KB | 0.1 KB | 0.0 KB |
| `fft` | 12.5 KB | 0.0 KB | 15.1 KB |
| `file_loader` | 11.7 KB | 0.0 KB | 0.0 KB |
| `minblep_table` | 1.4 KB | 0.0 KB | 16.0 KB |
| `minblep_state` | 0.3 KB | 0.0 KB | 0.0 KB |
| `cedar` | 0.2 KB | 0.0 KB | 0.0 KB |

### `minsize`

| Object | Text | Data | BSS |
|--------|-----:|-----:|----:|
| `audio_decoder` | 129.9 KB | 0.1 KB | 1.0 KB |
| `vm` | 77.3 KB | 0.0 KB | 0.0 KB |
| `soundfont` | 30.6 KB | 0.1 KB | 0.0 KB |
| `fft` | 5.5 KB | 0.0 KB | 15.1 KB |
| `file_loader` | 4.0 KB | 0.0 KB | 0.0 KB |
| `minblep_table` | 1.1 KB | 0.0 KB | 16.0 KB |
| `minblep_state` | 0.2 KB | 0.0 KB | 0.0 KB |
| `cedar` | 0.2 KB | 0.0 KB | 0.0 KB |

### `minsize-stripped`

| Object | Text | Data | BSS |
|--------|-----:|-----:|----:|
| `vm` | 71.9 KB | 0.0 KB | 0.0 KB |
| `cedar` | 0.2 KB | 0.0 KB | 0.0 KB |

### `esp32`

| Object | Text | Data | BSS |
|--------|-----:|-----:|----:|
| `vm` | 71.8 KB | 0.0 KB | 0.0 KB |
| `cedar` | 0.2 KB | 0.0 KB | 0.0 KB |

## Feature Reference

### Toggles

| Toggle | Default | Purpose |
|--------|---------|---------|
| `CEDAR_ENABLE_AUDIO_DECODERS` | ON | MP3/FLAC/OGG decoders (stb_vorbis, dr_flac, minimp3) |
| `CEDAR_ENABLE_SOUNDFONT` | ON | SoundFont (SF2) support via TinySoundFont |
| `CEDAR_ENABLE_FFT` | ON | FFT support (KissFFT) for FFT_PROBE opcode |
| `CEDAR_ENABLE_FILE_IO` | ON | std::filesystem-based file loading |
| `CEDAR_ENABLE_MINBLEP` | ON | MinBLEP anti-aliased oscillators |
| `CEDAR_ENABLE_FLOAT_ONLY` | OFF | Use float instead of double for beat timing |

_Note: `FLOAT_ONLY` is compiled as `-DCEDAR_FLOAT_ONLY` (behavior flag, not enable flag)._

### Memory Overrides

| Variable | Default | Purpose |
|----------|--------:|---------|
| `CEDAR_BLOCK_SIZE` | 128 | Samples per audio block |
| `CEDAR_MAX_BUFFERS` | 256 | Buffer pool size (registers) |
| `CEDAR_MAX_STATES` | 512 | DSP state pool size |
| `CEDAR_MAX_VARS` | 4096 | Variable slots |
| `CEDAR_MAX_PROGRAM_SIZE` | 4096 | Program bytecode capacity |
| `CEDAR_ARENA_SIZE` | 33554432 | AudioArena bytes (default 32 MB) |

# Cross-platform porting requirements

> **Status: VERIFIED** — Portability blockers surfaced in CI on 2026-04-22; source-level fixes applied the same day on `master` and confirmed green via godot-nkido-addon [run 24777116197 attempt 2](https://github.com/mlaass/godot-nkido-addon/actions/runs/24777116197) (all 6 matrix jobs pass across Linux / Windows / macOS × Debug / Release).

## Context

Discovered 2026-04-22 via CI in `godot-nkido-addon`
([run 24777116197](https://github.com/mlaass/godot-nkido-addon/actions/runs/24777116197)).

When building nkido as part of the Godot addon, **Ubuntu builds pass** (Debug + Release) but **Windows (MSVC) and macOS (Apple libc++)** both fail with portability issues in cedar and akkado. These are upstream nkido problems; the addon side is clean.

Three blockers, all in core sources:

---

## 1. `std::aligned_alloc` — MSVC

**File:** `cedar/include/cedar/vm/audio_arena.hpp:45`

**Error (MSVC 19.44):**
```
error C2039: 'aligned_alloc': is not a member of 'std'
error C3861: 'aligned_alloc': identifier not found
```

**Cause:** MSVC's `<cstdlib>` does not provide `std::aligned_alloc`. The function is C11 / C++17, but Microsoft's CRT has never implemented it (alignment requirement clashes with their `free` contract). POSIX has `posix_memalign`, which the header already handles behind `CEDAR_USE_POSIX_MEMALIGN`. Windows needs its own branch.

**Fix:** Add a third branch for MSVC using `_aligned_malloc` / `_aligned_free`. Example:

```cpp
#if defined(_MSC_VER)
    memory_ = static_cast<float*>(_aligned_malloc(aligned_size, ALIGNMENT));
#elif defined(CEDAR_USE_POSIX_MEMALIGN)
    // ... existing posix_memalign branch
#else
    memory_ = static_cast<float*>(std::aligned_alloc(ALIGNMENT, aligned_size));
#endif
```

Every free site must be updated — `_aligned_malloc` **requires** `_aligned_free`; plain `std::free` is undefined behavior. There are **two** such sites in `audio_arena.hpp`:

- Line 57 — destructor `~AudioArena()`
- Line 78 — move-assignment operator `AudioArena& operator=(AudioArena&&)`

The move-constructor (line 65) only steals the raw pointer, so it needs no change.

Alternative: drop the aligned-allocator entirely and use C++17's aligned `operator new` (`::operator new(size, std::align_val_t{ALIGNMENT})` / matching delete). Portable but slightly more verbose.

**Recommendation:** platform-branched `_aligned_malloc` / `_aligned_free`. The branches are already established in the file (`CEDAR_USE_POSIX_MEMALIGN`), so adding a third is consistent with existing style. Update both free sites.

---

## 2. `__builtin_ctz` — MSVC

**File:** `cedar/src/dsp/fft.cpp:41`

**Error (MSVC):**
```
error C3861: '__builtin_ctz': identifier not found
```

**Cause:** `__builtin_ctz` is a GCC/Clang intrinsic. MSVC has no such builtin (it has `_BitScanForward` via `<intrin.h>`).

**Fix:** Replace with C++20 `std::countr_zero` from `<bit>`. Already required by other parts of the codebase and works everywhere:

```cpp
#include <bit>
// ...
static int log2_size(std::size_t nfft) {
    return std::countr_zero(static_cast<unsigned>(nfft));
}
```

If C++20 is not an option here, guard the call:

```cpp
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, static_cast<unsigned long>(nfft));
    return static_cast<int>(idx);
#else
    return __builtin_ctz(static_cast<unsigned>(nfft));
#endif
```

**Recommendation:** `std::countr_zero`. The project already mandates C++20 (all three `CMakeLists.txt` set `CMAKE_CXX_STANDARD 20`), so the MSVC `_BitScanForward` guard is unnecessary in practice.

---

## 3. `std::from_chars<double>` — macOS libc++

**Files:**
- `akkado/src/lexer.cpp:361`
- `akkado/src/mini_lexer.cpp:355` (main number lexer)
- `akkado/src/mini_lexer.cpp:494, 555, 624` (three `:velocity` suffix parsers)

Integer `from_chars` overloads in `tuning.cpp` and the `var_val` sites in `mini_lexer.cpp` are unaffected — Apple libc++ only deletes the floating-point overloads.

**Error (Apple Clang, macos-latest runner):**
```
error: call to deleted function 'from_chars'
```

**Cause:** Apple's shipped libc++ kept the floating-point overloads of `std::from_chars` as `= delete` for years after they were added to the standard. Recent Xcode versions fixed it, but GitHub's `macos-latest` image may still hit an older SDK that trips this. Integer overloads work fine; only `double` / `float` are affected.

**Fix options (pick one):**

- **A. Unconditionally use `std::strtod`** for floats — less elegant but removes one `#ifdef` and avoids the whole category of libc++ FP-parsing bugs. Needs a null-terminated buffer:
  ```cpp
  std::string buf(text);
  char* end = nullptr;
  double value = std::strtod(buf.c_str(), &end);
  if (end == buf.c_str()) return make_error_token("Invalid number");
  ```

- **B. Bump the CI toolchain** — pin `macos-14` (or the `xcode-select` toolchain that ships libc++ with working `from_chars<double>`) instead of `macos-latest`. Kicks the can; future runners will eventually be fine but users building locally on older machines still break.

**Recommendation: A.** `std::from_chars` here is not on a hot path (lexer runs once at compile, not every audio block), so `std::strtod` is cheap and eliminates the libc++ version dependency entirely.

---

## Out of scope

- 32-bit targets, WASM builds, other compilers (ICC, NVCC) — these are not exercised by the addon's CI today.
- macOS universal binary / arm64 builds — the addon's CI uses `macos-latest` single-arch; separate concern.

## Verification

Verified 2026-04-23 by re-running the failed workflow against current nkido master (`a483261`). Since `.github/workflows/build.yml` in `godot-nkido-addon` checks out nkido via `actions/checkout@v4` with no `ref`, `gh run rerun <id> --failed` is enough to pick up the fix — no addon commit needed.

All 6 matrix jobs green on [attempt 2](https://github.com/mlaass/godot-nkido-addon/actions/runs/24777116197):

- ubuntu-latest / Debug, Release ✓
- windows-latest / Debug, Release ✓ (was failing on items 1 + 2)
- macos-latest / Debug, Release ✓ (was failing on item 3)

# Cross-platform porting requirements

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

The destructor (line 57) also needs a matching branch — `_aligned_malloc` **requires** `_aligned_free`; plain `std::free` is undefined behavior.

Alternative: drop the aligned-allocator entirely and use C++17's aligned `operator new` (`::operator new(size, std::align_val_t{ALIGNMENT})` / matching delete). Portable but slightly more verbose.

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

`std::countr_zero` is the cleaner option.

---

## 3. `std::from_chars<double>` — macOS libc++

**File:** `akkado/src/lexer.cpp:361`

**Error (Apple Clang, macos-latest runner):**
```
error: call to deleted function 'from_chars'
```

**Cause:** Apple's shipped libc++ kept the floating-point overloads of `std::from_chars` as `= delete` for years after they were added to the standard. Recent Xcode versions fixed it, but GitHub's `macos-latest` image may still hit an older SDK that trips this. Integer overloads work fine; only `double` / `float` are affected.

**Fix options (pick one):**

- **A. Fallback for Apple libc++ only** — detect the broken shipping and route doubles through `std::strtod` (needs a null-terminated buffer):
  ```cpp
  #if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 170000
      std::string buf(text);
      char* end = nullptr;
      double value = std::strtod(buf.c_str(), &end);
      if (end == buf.c_str()) return make_error_token("Invalid number");
  #else
      double value = 0.0;
      auto result = std::from_chars(text.data(), text.data() + text.size(), value);
      if (result.ec != std::errc{}) return make_error_token("Invalid number");
  #endif
  ```

- **B. Unconditionally use `std::strtod`** for floats — less elegant but removes one `#ifdef` and avoids the whole category of libc++ FP-parsing bugs.

- **C. Bump the CI toolchain** — pin `macos-14` (or the `xcode-select` toolchain that ships libc++ with working `from_chars<double>`) instead of `macos-latest`. Kicks the can; future runners will eventually be fine but users building locally on older machines still break.

**Recommendation: B.** `std::from_chars` here is not on a hot path (lexer runs once at compile, not every audio block), so `std::strtod` is cheap and eliminates the libc++ version dependency entirely.

---

## Out of scope

- 32-bit targets, WASM builds, other compilers (ICC, NVCC) — these are not exercised by the addon's CI today.
- macOS universal binary / arm64 builds — the addon's CI uses `macos-latest` single-arch; separate concern.

## Verification

After each fix, push to a branch of `godot-nkido-addon` with this commit of nkido checked out, and confirm all 6 matrix jobs in
`.github/workflows/build.yml` go green:

- ubuntu-latest / Debug, Release
- windows-latest / Debug, Release (currently failing on items 1 + 2)
- macos-latest / Debug, Release (currently failing on item 3)

The CI configure step passes on all three OSes — so the build system is already portable; only the source-level fixes above remain.

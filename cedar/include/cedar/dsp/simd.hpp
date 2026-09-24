#pragma once

// Runtime SIMD dispatch (docs/prd-simd-autoresearch.md §5.1).
//
// Opcodes keep their scalar body as oracle + fallback and add kernels behind
// `if (simd::has(...))`. Kernels only exist when the build sets CEDAR_SIMD
// (the `simd` / `wasm-simd` presets); default builds compile to the scalar
// bodies exactly as before.
//
//   x86-64 (GCC/Clang): kernels carry CEDAR_TARGET_AVX2, so no TU needs
//     -mavx2; the ISA is detected once at static init. SSE2 is baseline.
//   aarch64: NEON is baseline — CEDAR_SIMD_NEON kernels need no dispatch.
//   WASM:    SIMD128 when built with -msimd128 (CEDAR_SIMD_WASM).
//
// Kernels must not allocate, lock or syscall (the [zero_alloc] trap enforces),
// and may assume 32-byte aligned buffers (BufferPool slabs are alignas(32)) —
// assert it with CEDAR_SIMD_ASSERT_ALIGNED.

#include <cassert>
#include <cstdint>

#if defined(CEDAR_SIMD) && (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#  define CEDAR_SIMD_X86 1
#  include <immintrin.h>
#  define CEDAR_TARGET_AVX2 [[gnu::target("avx2")]]
#else
#  define CEDAR_SIMD_X86 0
#endif

#if defined(CEDAR_SIMD) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#  define CEDAR_SIMD_NEON 1
#  include <arm_neon.h>
#else
#  define CEDAR_SIMD_NEON 0
#endif

#if defined(CEDAR_SIMD) && defined(__wasm_simd128__)
#  define CEDAR_SIMD_WASM 1
#  include <wasm_simd128.h>
#else
#  define CEDAR_SIMD_WASM 0
#endif

#define CEDAR_SIMD_ASSERT_ALIGNED(p, n) \
    assert((reinterpret_cast<std::uintptr_t>(p) % (n)) == 0 && "SIMD kernel: misaligned buffer")

namespace cedar::simd {

enum class Isa : std::uint8_t { Scalar, Sse2, Avx2, Neon, Wasm128 };

namespace detail {
// Written once during static initialisation (src/dsp/simd.cpp); read-only after.
extern Isa g_active;
}

// Best ISA available on this CPU *and* compiled into this build.
[[nodiscard]] inline Isa active() noexcept { return detail::g_active; }

// True when kernels for `isa` may run. x86 levels nest (Avx2 implies Sse2).
[[nodiscard]] inline bool has(Isa isa) noexcept {
    const Isa a = detail::g_active;
    if (isa == Isa::Scalar) return true;
    if (isa == Isa::Sse2) return a == Isa::Sse2 || a == Isa::Avx2;
    return a == isa;
}

[[nodiscard]] const char* name(Isa isa) noexcept;

// Test/bench hook: force a lower ISA (e.g. Scalar) to A/B the fallback path.
// Not for use while audio is running.
void force(Isa isa) noexcept;

}  // namespace cedar::simd

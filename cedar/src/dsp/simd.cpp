#include "cedar/dsp/simd.hpp"

namespace cedar::simd {

namespace {
Isa detect() noexcept {
#if CEDAR_SIMD_X86
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) return Isa::Avx2;
    if (__builtin_cpu_supports("sse2")) return Isa::Sse2;
    return Isa::Scalar;
#elif CEDAR_SIMD_NEON
    return Isa::Neon;
#elif CEDAR_SIMD_WASM
    return Isa::Wasm128;
#else
    return Isa::Scalar;
#endif
}
}  // namespace

Isa detail::g_active = detect();

const char* name(Isa isa) noexcept {
    switch (isa) {
        case Isa::Scalar: return "scalar";
        case Isa::Sse2: return "sse2";
        case Isa::Avx2: return "avx2";
        case Isa::Neon: return "neon";
        case Isa::Wasm128: return "wasm128";
    }
    return "unknown";
}

void force(Isa isa) noexcept { detail::g_active = isa; }

}  // namespace cedar::simd

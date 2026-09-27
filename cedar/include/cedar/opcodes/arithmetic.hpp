#pragma once

#include "../dsp/simd.hpp"

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include <cmath>

namespace cedar {

namespace kernels {
#if CEDAR_SIMD_X86
#define CEDAR_BINARY_AVX2_KERNEL(name, intrinsic)                                      \
    CEDAR_TARGET_AVX2 inline void name(const float* a, const float* b, float* out)     \
        noexcept {                                                                     \
        const __m256 a0 = _mm256_load_ps(a + 0);                                      \
        const __m256 b0 = _mm256_load_ps(b + 0);                                      \
        const __m256 a1 = _mm256_load_ps(a + 8);                                      \
        const __m256 b1 = _mm256_load_ps(b + 8);                                      \
        const __m256 a2 = _mm256_load_ps(a + 16);                                     \
        const __m256 b2 = _mm256_load_ps(b + 16);                                     \
        const __m256 a3 = _mm256_load_ps(a + 24);                                     \
        const __m256 b3 = _mm256_load_ps(b + 24);                                     \
        _mm256_store_ps(out + 0, intrinsic(a0, b0));                                  \
        _mm256_store_ps(out + 8, intrinsic(a1, b1));                                  \
        _mm256_store_ps(out + 16, intrinsic(a2, b2));                                 \
        _mm256_store_ps(out + 24, intrinsic(a3, b3));                                 \
        const __m256 a4 = _mm256_load_ps(a + 32);                                     \
        const __m256 b4 = _mm256_load_ps(b + 32);                                     \
        const __m256 a5 = _mm256_load_ps(a + 40);                                     \
        const __m256 b5 = _mm256_load_ps(b + 40);                                     \
        const __m256 a6 = _mm256_load_ps(a + 48);                                     \
        const __m256 b6 = _mm256_load_ps(b + 48);                                     \
        const __m256 a7 = _mm256_load_ps(a + 56);                                     \
        const __m256 b7 = _mm256_load_ps(b + 56);                                     \
        _mm256_store_ps(out + 32, intrinsic(a4, b4));                                 \
        _mm256_store_ps(out + 40, intrinsic(a5, b5));                                 \
        _mm256_store_ps(out + 48, intrinsic(a6, b6));                                 \
        _mm256_store_ps(out + 56, intrinsic(a7, b7));                                 \
        const __m256 a8 = _mm256_load_ps(a + 64);                                     \
        const __m256 b8 = _mm256_load_ps(b + 64);                                     \
        const __m256 a9 = _mm256_load_ps(a + 72);                                     \
        const __m256 b9 = _mm256_load_ps(b + 72);                                     \
        const __m256 a10 = _mm256_load_ps(a + 80);                                    \
        const __m256 b10 = _mm256_load_ps(b + 80);                                    \
        const __m256 a11 = _mm256_load_ps(a + 88);                                    \
        const __m256 b11 = _mm256_load_ps(b + 88);                                    \
        _mm256_store_ps(out + 64, intrinsic(a8, b8));                                 \
        _mm256_store_ps(out + 72, intrinsic(a9, b9));                                 \
        _mm256_store_ps(out + 80, intrinsic(a10, b10));                               \
        _mm256_store_ps(out + 88, intrinsic(a11, b11));                               \
        const __m256 a12 = _mm256_load_ps(a + 96);                                    \
        const __m256 b12 = _mm256_load_ps(b + 96);                                    \
        const __m256 a13 = _mm256_load_ps(a + 104);                                   \
        const __m256 b13 = _mm256_load_ps(b + 104);                                   \
        const __m256 a14 = _mm256_load_ps(a + 112);                                   \
        const __m256 b14 = _mm256_load_ps(b + 112);                                   \
        const __m256 a15 = _mm256_load_ps(a + 120);                                   \
        const __m256 b15 = _mm256_load_ps(b + 120);                                   \
        _mm256_store_ps(out + 96, intrinsic(a12, b12));                               \
        _mm256_store_ps(out + 104, intrinsic(a13, b13));                              \
        _mm256_store_ps(out + 112, intrinsic(a14, b14));                              \
        _mm256_store_ps(out + 120, intrinsic(a15, b15));                              \
    }

CEDAR_BINARY_AVX2_KERNEL(add_avx2, _mm256_add_ps)
CEDAR_BINARY_AVX2_KERNEL(sub_avx2, _mm256_sub_ps)
CEDAR_BINARY_AVX2_KERNEL(mul_avx2, _mm256_mul_ps)

#undef CEDAR_BINARY_AVX2_KERNEL
#endif
} // namespace kernels

// ADD: out = in0 + in1
[[gnu::always_inline]]
inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::add_avx2(a, b, out);
        return;
    }
#endif
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = a[i] + b[i];
    }
}

// SUB: out = in0 - in1
[[gnu::always_inline]]
inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::sub_avx2(a, b, out);
        return;
    }
#endif
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = a[i] - b[i];
    }
}

// MUL: out = in0 * in1
[[gnu::always_inline]]
inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::mul_avx2(a, b, out);
        return;
    }
#endif
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = a[i] * b[i];
    }
}

// DIV: out = in0 / in1 (safe division, returns 0 for 0/0)
[[gnu::always_inline]]
inline void op_div(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = (b[i] != 0.0f) ? (a[i] / b[i]) : 0.0f;
    }
}

// POW: out = in0 ^ in1
[[gnu::always_inline]]
inline void op_pow(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = std::pow(a[i], b[i]);
    }
}

// NEG: out = -in0
[[gnu::always_inline]]
inline void op_neg(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = -a[i];
    }
}

}  // namespace cedar

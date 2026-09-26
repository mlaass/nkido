#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "../dsp/simd.hpp"
#include <cmath>

namespace cedar {

namespace kernels {
#if CEDAR_SIMD_X86
CEDAR_TARGET_AVX2 inline void add_avx2(const float* __restrict a, const float* __restrict b, float* __restrict out) noexcept {
    a = static_cast<const float*>(__builtin_assume_aligned(a, 32));
    b = static_cast<const float*>(__builtin_assume_aligned(b, 32));
    out = static_cast<float*>(__builtin_assume_aligned(out, 32));
    #pragma GCC unroll 4
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
    }
}

CEDAR_TARGET_AVX2 inline void sub_avx2(const float* __restrict a, const float* __restrict b, float* __restrict out) noexcept {
    a = static_cast<const float*>(__builtin_assume_aligned(a, 32));
    b = static_cast<const float*>(__builtin_assume_aligned(b, 32));
    out = static_cast<float*>(__builtin_assume_aligned(out, 32));
    #pragma GCC unroll 4
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
    }
}

CEDAR_TARGET_AVX2 inline void mul_avx2(const float* __restrict a, const float* __restrict b, float* __restrict out) noexcept {
    a = static_cast<const float*>(__builtin_assume_aligned(a, 32));
    b = static_cast<const float*>(__builtin_assume_aligned(b, 32));
    out = static_cast<float*>(__builtin_assume_aligned(out, 32));
    #pragma GCC unroll 4
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
    }
}
#endif

#if CEDAR_SIMD_NEON
inline void add_neon(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        vst1q_f32(out + i, vaddq_f32(va, vb));
    }
}

inline void sub_neon(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        vst1q_f32(out + i, vsubq_f32(va, vb));
    }
}

inline void mul_neon(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        vst1q_f32(out + i, vmulq_f32(va, vb));
    }
}
#endif

#if CEDAR_SIMD_WASM
inline void add_wasm(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        v128_t va = wasm_v128_load(a + i);
        v128_t vb = wasm_v128_load(b + i);
        wasm_v128_store(out + i, wasm_f32x4_add(va, vb));
    }
}

inline void sub_wasm(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        v128_t va = wasm_v128_load(a + i);
        v128_t vb = wasm_v128_load(b + i);
        wasm_v128_store(out + i, wasm_f32x4_sub(va, vb));
    }
}

inline void mul_wasm(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        v128_t va = wasm_v128_load(a + i);
        v128_t vb = wasm_v128_load(b + i);
        wasm_v128_store(out + i, wasm_f32x4_mul(va, vb));
    }
}
#endif
}

// ADD: out = in0 + in1
[[gnu::always_inline]]
inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
#endif
#if CEDAR_SIMD_NEON
    kernels::add_neon(a, b, out); return;
#endif
#if CEDAR_SIMD_WASM
    kernels::add_wasm(a, b, out); return;
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
    if (simd::has(simd::Isa::Avx2)) { kernels::sub_avx2(a, b, out); return; }
#endif
#if CEDAR_SIMD_NEON
    kernels::sub_neon(a, b, out); return;
#endif
#if CEDAR_SIMD_WASM
    kernels::sub_wasm(a, b, out); return;
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
    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
#endif
#if CEDAR_SIMD_NEON
    kernels::mul_neon(a, b, out); return;
#endif
#if CEDAR_SIMD_WASM
    kernels::mul_wasm(a, b, out); return;
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

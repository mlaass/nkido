#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "../dsp/simd.hpp"
#include <cmath>

namespace cedar {

// --- SIMD kernels: ADD / SUB / MUL ----------------------------------------
//
// Element-wise IEEE-754 add/sub/mul over one block, widened from the scalar
// bodies below (which stay as oracle + fallback). add/sub/mul are single
// rounded operations per element, so 8-lane AVX2 / 4-lane SSE2-NEON-WASM
// code is bit-identical to the scalar loop for *every* input — denormals,
// infinities, NaNs and signed zeros included. No FMA (would change
// rounding), no FTZ/DAZ (MXCSR is left untouched).
//
// Kernels may assume 32-byte aligned buffers (BufferPool slabs are
// alignas(32)) and BLOCK_SIZE is a compile-time multiple of the lane count,
// so each loop fully unrolls. No allocation, lock or syscall; the ISA check
// in the opcode reads a global written once at static init.
namespace kernels {

#if CEDAR_SIMD_X86
// AVX2: 8 lanes, aligned loads/stores.
//
// The output buffer's 8 cache lines are almost always cold in L1 when the
// kernel runs (a program typically writes dozens of distinct buffers per
// block, far more than L1D can hold), and an aligned 32-byte store covers
// only half a 64-byte line — so every store miss triggers a write-allocate
// L2->L1 line fetch that the store buffer has to wait behind. Prefetching
// all 8 output lines up front starts those fills while the kernel is still
// busy with its 32 input loads, moving the fetch off the stores' critical
// path. Prefetches are pure hardware hints: no architectural state, no
// effect on the computed values, exact for every input.
namespace detail {
// 8 prefetcht0 ops, one per 64-byte line of the block. BLOCK_SIZE floats
// at 4 bytes each: 512 bytes = 8 lines.
CEDAR_TARGET_AVX2 inline void prefetch_out_avx2(const float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 16) {
        _mm_prefetch(reinterpret_cast<const char*>(out + i), _MM_HINT_T0);
    }
}
}  // namespace detail

CEDAR_TARGET_AVX2 inline void add_avx2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
    detail::prefetch_out_avx2(out);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
    }
}

CEDAR_TARGET_AVX2 inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
    detail::prefetch_out_avx2(out);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
    }
}

CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
    detail::prefetch_out_avx2(out);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
    }
}

// SSE2: 4 lanes, for x86-64 hosts without AVX2 (SSE2 is baseline).
inline void add_sse2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 16);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 16);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 16);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        _mm_store_ps(out + i, _mm_add_ps(_mm_load_ps(a + i), _mm_load_ps(b + i)));
    }
}

inline void sub_sse2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 16);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 16);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 16);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        _mm_store_ps(out + i, _mm_sub_ps(_mm_load_ps(a + i), _mm_load_ps(b + i)));
    }
}

inline void mul_sse2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 16);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 16);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 16);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        _mm_store_ps(out + i, _mm_mul_ps(_mm_load_ps(a + i), _mm_load_ps(b + i)));
    }
}
#endif  // CEDAR_SIMD_X86

#if CEDAR_SIMD_NEON
// aarch64: NEON is baseline, 4 lanes.
inline void add_neon(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        vst1q_f32(out + i, vaddq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    }
}

inline void sub_neon(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        vst1q_f32(out + i, vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    }
}

inline void mul_neon(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        vst1q_f32(out + i, vmulq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    }
}
#endif  // CEDAR_SIMD_NEON

#if CEDAR_SIMD_WASM
// WASM SIMD128: 4 lanes.
inline void add_wasm(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        wasm_v128_store(out + i, wasm_f32x4_add(wasm_v128_load(a + i), wasm_v128_load(b + i)));
    }
}

inline void sub_wasm(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        wasm_v128_store(out + i, wasm_f32x4_sub(wasm_v128_load(a + i), wasm_v128_load(b + i)));
    }
}

inline void mul_wasm(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        wasm_v128_store(out + i, wasm_f32x4_mul(wasm_v128_load(a + i), wasm_v128_load(b + i)));
    }
}
#endif  // CEDAR_SIMD_WASM

}  // namespace kernels

// ADD: out = in0 + in1
[[gnu::always_inline]]
inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
    if (simd::has(simd::Isa::Sse2)) { kernels::add_sse2(a, b, out); return; }
#elif CEDAR_SIMD_NEON
    if (simd::has(simd::Isa::Neon)) { kernels::add_neon(a, b, out); return; }
#elif CEDAR_SIMD_WASM
    if (simd::has(simd::Isa::Wasm128)) { kernels::add_wasm(a, b, out); return; }
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
    if (simd::has(simd::Isa::Sse2)) { kernels::sub_sse2(a, b, out); return; }
#elif CEDAR_SIMD_NEON
    if (simd::has(simd::Isa::Neon)) { kernels::sub_neon(a, b, out); return; }
#elif CEDAR_SIMD_WASM
    if (simd::has(simd::Isa::Wasm128)) { kernels::sub_wasm(a, b, out); return; }
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
    if (simd::has(simd::Isa::Sse2)) { kernels::mul_sse2(a, b, out); return; }
#elif CEDAR_SIMD_NEON
    if (simd::has(simd::Isa::Neon)) { kernels::mul_neon(a, b, out); return; }
#elif CEDAR_SIMD_WASM
    if (simd::has(simd::Isa::Wasm128)) { kernels::mul_wasm(a, b, out); return; }
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

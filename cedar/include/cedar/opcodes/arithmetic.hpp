#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "../dsp/simd.hpp"
#include <cmath>

namespace cedar {

namespace kernels {
#if CEDAR_SIMD_X86
// Element-wise binary ops. Each 8-lane vector is fully loaded before being
// stored, so in-place use (out == a or out == b) is safe.
//
// The output block is 8 cache lines; when it is not L1-resident each store
// needs an RFO, and the store buffer would otherwise wait on them one line
// at a time. prefetchw on all 8 lines up front overlaps those RFOs with the
// loads and arithmetic. (prfchw ships on every AVX2 CPU; it's a hint only.)
#define CEDAR_TARGET_AVX2_PRFCHW [[gnu::target("avx2,prfchw")]]
CEDAR_TARGET_AVX2_PRFCHW inline void prefetch_block_for_write(float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 16) _m_prefetchw(out + i);
}

CEDAR_TARGET_AVX2_PRFCHW inline void add_avx2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
    prefetch_block_for_write(out);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
}

CEDAR_TARGET_AVX2_PRFCHW inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
    prefetch_block_for_write(out);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
}

CEDAR_TARGET_AVX2_PRFCHW inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
    prefetch_block_for_write(out);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
}
#undef CEDAR_TARGET_AVX2_PRFCHW
#endif
}  // namespace kernels

// ADD: out = in0 + in1
[[gnu::always_inline]]
inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* a = ctx.buffers->get(inst.inputs[0]);
    const float* b = ctx.buffers->get(inst.inputs[1]);

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
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

#pragma once

#include "../dsp/simd.hpp"

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include <cmath>

namespace cedar {

namespace kernels {
#if CEDAR_SIMD_X86
#define CEDAR_BINARY_AVX2_KERNEL(name, instruction)                                   \
    [[gnu::always_inline]] inline void name(const float* a, const float* b,           \
                                             float* out) noexcept {                    \
        asm volatile(                                                                 \
            "vmovaps 0(%[a]), %%ymm0\n\t"                                             \
            "vmovaps 32(%[a]), %%ymm1\n\t"                                            \
            "vmovaps 64(%[a]), %%ymm2\n\t"                                            \
            "vmovaps 96(%[a]), %%ymm3\n\t"                                            \
            instruction " 0(%[b]), %%ymm0, %%ymm0\n\t"                              \
            instruction " 32(%[b]), %%ymm1, %%ymm1\n\t"                             \
            instruction " 64(%[b]), %%ymm2, %%ymm2\n\t"                             \
            instruction " 96(%[b]), %%ymm3, %%ymm3\n\t"                             \
            "vmovaps %%ymm0, 0(%[out])\n\t"                                          \
            "vmovaps %%ymm1, 32(%[out])\n\t"                                         \
            "vmovaps %%ymm2, 64(%[out])\n\t"                                         \
            "vmovaps %%ymm3, 96(%[out])\n\t"                                         \
            "vmovaps 128(%[a]), %%ymm0\n\t"                                         \
            "vmovaps 160(%[a]), %%ymm1\n\t"                                         \
            "vmovaps 192(%[a]), %%ymm2\n\t"                                         \
            "vmovaps 224(%[a]), %%ymm3\n\t"                                         \
            instruction " 128(%[b]), %%ymm0, %%ymm0\n\t"                           \
            instruction " 160(%[b]), %%ymm1, %%ymm1\n\t"                           \
            instruction " 192(%[b]), %%ymm2, %%ymm2\n\t"                           \
            instruction " 224(%[b]), %%ymm3, %%ymm3\n\t"                           \
            "vmovaps %%ymm0, 128(%[out])\n\t"                                        \
            "vmovaps %%ymm1, 160(%[out])\n\t"                                        \
            "vmovaps %%ymm2, 192(%[out])\n\t"                                        \
            "vmovaps %%ymm3, 224(%[out])\n\t"                                        \
            "vmovaps 256(%[a]), %%ymm0\n\t"                                         \
            "vmovaps 288(%[a]), %%ymm1\n\t"                                         \
            "vmovaps 320(%[a]), %%ymm2\n\t"                                         \
            "vmovaps 352(%[a]), %%ymm3\n\t"                                         \
            instruction " 256(%[b]), %%ymm0, %%ymm0\n\t"                           \
            instruction " 288(%[b]), %%ymm1, %%ymm1\n\t"                           \
            instruction " 320(%[b]), %%ymm2, %%ymm2\n\t"                           \
            instruction " 352(%[b]), %%ymm3, %%ymm3\n\t"                           \
            "vmovaps %%ymm0, 256(%[out])\n\t"                                        \
            "vmovaps %%ymm1, 288(%[out])\n\t"                                        \
            "vmovaps %%ymm2, 320(%[out])\n\t"                                        \
            "vmovaps %%ymm3, 352(%[out])\n\t"                                        \
            "vmovaps 384(%[a]), %%ymm0\n\t"                                         \
            "vmovaps 416(%[a]), %%ymm1\n\t"                                         \
            "vmovaps 448(%[a]), %%ymm2\n\t"                                         \
            "vmovaps 480(%[a]), %%ymm3\n\t"                                         \
            instruction " 384(%[b]), %%ymm0, %%ymm0\n\t"                           \
            instruction " 416(%[b]), %%ymm1, %%ymm1\n\t"                           \
            instruction " 448(%[b]), %%ymm2, %%ymm2\n\t"                           \
            instruction " 480(%[b]), %%ymm3, %%ymm3\n\t"                           \
            "vmovaps %%ymm0, 384(%[out])\n\t"                                        \
            "vmovaps %%ymm1, 416(%[out])\n\t"                                        \
            "vmovaps %%ymm2, 448(%[out])\n\t"                                        \
            "vmovaps %%ymm3, 480(%[out])\n\t"                                        \
            "vzeroupper"                                                               \
            :                                                                          \
            : [a] "r"(a), [b] "r"(b), [out] "r"(out)                                \
            : "ymm0", "ymm1", "ymm2", "ymm3", "memory");                         \
    }

CEDAR_BINARY_AVX2_KERNEL(add_avx2, "vaddps")
CEDAR_BINARY_AVX2_KERNEL(sub_avx2, "vsubps")

[[gnu::always_inline]] inline void mul_avx2(const float* a, const float* b,
                                             float* out) noexcept {
    unsigned int chunks = 4;
    asm volatile(
        "1:\n\t"
        "vmovaps 0(%[a]), %%ymm0\n\t"
        "vmovaps 32(%[a]), %%ymm1\n\t"
        "vmovaps 64(%[a]), %%ymm2\n\t"
        "vmovaps 96(%[a]), %%ymm3\n\t"
        "vmulps 0(%[b]), %%ymm0, %%ymm0\n\t"
        "vmulps 32(%[b]), %%ymm1, %%ymm1\n\t"
        "vmulps 64(%[b]), %%ymm2, %%ymm2\n\t"
        "vmulps 96(%[b]), %%ymm3, %%ymm3\n\t"
        "vmovaps %%ymm0, 0(%[out])\n\t"
        "vmovaps %%ymm1, 32(%[out])\n\t"
        "vmovaps %%ymm2, 64(%[out])\n\t"
        "vmovaps %%ymm3, 96(%[out])\n\t"
        "add $128, %[a]\n\t"
        "add $128, %[b]\n\t"
        "add $128, %[out]\n\t"
        "dec %[chunks]\n\t"
        "jnz 1b\n\t"
        "vzeroupper"
        : [a] "+r"(a), [b] "+r"(b), [out] "+r"(out),
          [chunks] "+r"(chunks)
        :
        : "ymm0", "ymm1", "ymm2", "ymm3", "cc", "memory");
}

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

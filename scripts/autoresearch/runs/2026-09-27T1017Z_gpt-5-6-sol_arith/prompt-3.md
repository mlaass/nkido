You are optimising Cedar DSP opcodes with SIMD. Cedar is the real-time audio
engine of nkido: a bytecode VM that processes blocks of 128 float samples at
48 kHz, zero allocations on the audio path. The repository is your current
working directory.

## Task

Make these opcodes faster on **x86-64 AVX2** (the gate machine):

- `op_add`
- `op_sub`
- `op_mul`

They live in `cedar/include/cedar/opcodes/arithmetic.hpp`. Current source of the opcode bodies:

```cpp
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
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_add` | 14.47 | 11.5 | 1.26x |
| `op_sub` | 14.54 | 11.52 | 1.26x |
| `op_mul` | 14.47 | 11.46 | 1.26x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp b/cedar/include/cedar/opcodes/arithmetic.hpp
index 72deca0..ec4b2e6 100644
--- a/cedar/include/cedar/opcodes/arithmetic.hpp
+++ b/cedar/include/cedar/opcodes/arithmetic.hpp
@@ -1,5 +1,7 @@
 #pragma once
 
+#include "../dsp/simd.hpp"
+
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
@@ -7,6 +9,74 @@
 
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+#define CEDAR_BINARY_AVX2_KERNEL(name, instruction)                                   \
+    [[gnu::always_inline]] inline void name(const float* a, const float* b,           \
+                                             float* out) noexcept {                    \
+        asm volatile(                                                                 \
+            "vmovaps 0(%[a]), %%ymm0\n\t"                                             \
+            "vmovaps 32(%[a]), %%ymm1\n\t"                                            \
+            "vmovaps 64(%[a]), %%ymm2\n\t"                                            \
+            "vmovaps 96(%[a]), %%ymm3\n\t"                                            \
+            instruction " 0(%[b]), %%ymm0, %%ymm0\n\t"                              \
+            instruction " 32(%[b]), %%ymm1, %%ymm1\n\t"                             \
+            instruction " 64(%[b]), %%ymm2, %%ymm2\n\t"                             \
+            instruction " 96(%[b]), %%ymm3, %%ymm3\n\t"                             \
+            "vmovaps %%ymm0, 0(%[out])\n\t"                                          \
+            "vmovaps %%ymm1, 32(%[out])\n\t"                                         \
+            "vmovaps %%ymm2, 64(%[out])\n\t"                                         \
+            "vmovaps %%ymm3, 96(%[out])\n\t"                                         \
+            "vmovaps 128(%[a]), %%ymm0\n\t"                                         \
+            "vmovaps 160(%[a]), %%ymm1\n\t"                                         \
+            "vmovaps 192(%[a]), %%ymm2\n\t"                                         \
+            "vmovaps 224(%[a]), %%ymm3\n\t"                                         \
+            instruction " 128(%[b]), %%ymm0, %%ymm0\n\t"                           \
+            instruction " 160(%[b]), %%ymm1, %%ymm1\n\t"                           \
+            instruction " 192(%[b]), %%ymm2, %%ymm2\n\t"                           \
+            instruction " 224(%[b]), %%ymm3, %%ymm3\n\t"                           \
+            "vmovaps %%ymm0, 128(%[out])\n\t"                                        \
+            "vmovaps %%ymm1, 160(%[out])\n\t"                                        \
+            "vmovaps %%ymm2, 192(%[out])\n\t"                                        \
+            "vmovaps %%ymm3, 224(%[out])\n\t"                                        \
+            "vmovaps 256(%[a]), %%ymm0\n\t"                                         \
+            "vmovaps 288(%[a]), %%ymm1\n\t"                                         \
+            "vmovaps 320(%[a]), %%ymm2\n\t"                                         \
+            "vmovaps 352(%[a]), %%ymm3\n\t"                                         \
+            instruction " 256(%[b]), %%ymm0, %%ymm0\n\t"                           \
+            instruction " 288(%[b]), %%ymm1, %%ymm1\n\t"                           \
+            instruction " 320(%[b]), %%ymm2, %%ymm2\n\t"                           \
+            instruction " 352(%[b]), %%ymm3, %%ymm3\n\t"                           \
+            "vmovaps %%ymm0, 256(%[out])\n\t"                                        \
+            "vmovaps %%ymm1, 288(%[out])\n\t"                                        \
+            "vmovaps %%ymm2, 320(%[out])\n\t"                                        \
+            "vmovaps %%ymm3, 352(%[out])\n\t"                                        \
+            "vmovaps 384(%[a]), %%ymm0\n\t"                                         \
+            "vmovaps 416(%[a]), %%ymm1\n\t"                                         \
+            "vmovaps 448(%[a]), %%ymm2\n\t"                                         \
+            "vmovaps 480(%[a]), %%ymm3\n\t"                                         \
+            instruction " 384(%[b]), %%ymm0, %%ymm0\n\t"                           \
+            instruction " 416(%[b]), %%ymm1, %%ymm1\n\t"                           \
+            instruction " 448(%[b]), %%ymm2, %%ymm2\n\t"                           \
+            instruction " 480(%[b]), %%ymm3, %%ymm3\n\t"                           \
+            "vmovaps %%ymm0, 384(%[out])\n\t"                                        \
+            "vmovaps %%ymm1, 416(%[out])\n\t"                                        \
+            "vmovaps %%ymm2, 448(%[out])\n\t"                                        \
+            "vmovaps %%ymm3, 480(%[out])\n\t"                                        \
+            "vzeroupper"                                                               \
+            :                                                                          \
+            : [a] "r"(a), [b] "r"(b), [out] "r"(out)                                \
+            : "ymm0", "ymm1", "ymm2", "ymm3", "memory");                         \
+    }
+
+CEDAR_BINARY_AVX2_KERNEL(add_avx2, "vaddps")
+CEDAR_BINARY_AVX2_KERNEL(sub_avx2, "vsubps")
+CEDAR_BINARY_AVX2_KERNEL(mul_avx2, "vmulps")
+
+#undef CEDAR_BINARY_AVX2_KERNEL
+#endif
+} // namespace kernels
+
 // ADD: out = in0 + in1
 [[gnu::always_inline]]
 inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
@@ -14,6 +84,12 @@ inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::add_avx2(a, b, out);
+        return;
+    }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] + b[i];
     }
@@ -26,6 +102,12 @@ inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::sub_avx2(a, b, out);
+        return;
+    }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] - b[i];
     }
@@ -38,6 +120,12 @@ inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::mul_avx2(a, b, out);
+        return;
+    }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] * b[i];
     }
```

## How to add a kernel

The dispatch header `cedar/include/cedar/dsp/simd.hpp` is already there. Keep
the scalar body and add a kernel in front of it:

```cpp
#include "../dsp/simd.hpp"

namespace kernels {
#if CEDAR_SIMD_X86
CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
}
#endif
}

inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
    // ... resolve a, b, out ...
#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
#endif
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = a[i] * b[i];  // fallback stays
}
```

- `CEDAR_TARGET_AVX2` is `[[gnu::target("avx2")]]`; no compiler flags change.
  FMA is **not** enabled (it would change rounding).
- Buffers from `ctx.buffers->get()` are 32-byte aligned; `BLOCK_SIZE` is 128.
- You may also add NEON (`CEDAR_SIMD_NEON`) or WASM SIMD128
  (`CEDAR_SIMD_WASM`) paths; they are measured later on other machines, not
  gated here.

## Rules (enforced by the verifier — violations are rejected automatically)

1. You may only edit: `cedar/include/cedar/opcodes/**`, `cedar/include/cedar/dsp/simd*.hpp`, `cedar/src/dsp/simd*.cpp`. Any other edit (benchmarks, tests,
   experiments, CMake, scripts) rejects the whole attempt.
2. **Never delete or change the scalar body.** It is the oracle and the
   fallback; the verifier runs it with SIMD disabled and requires
   bit-identical output to the original.
3. Output must be **bit-identical** to the original for every opcode.
   No approximations are allowed for these opcodes.
4. The final verification also runs every opcode on **randomised stimuli you
   cannot see** (different signals, levels down to denormals, parameter
   trajectories). A kernel that only works for the stimuli in the benchmark
   source fails there.
5. No allocation, locks or syscalls in a kernel. CPU detection is already done
   once at start-up; never detect features per block or per sample.
6. The change must make at least one listed opcode **≥ 5 % faster** than the
   *current* version (outside measurement noise) and none slower.

## Budget

At most **10 runs of the check command** and **30 minutes** for
this attempt. Every model in this experiment gets the same budget.

Your tools are: read, write and edit files, search the repository, and run
the check command. **The check command is the only shell command allowed**
(piping its output, e.g. through `tail`, is fine). You cannot run Python,
compilers or other scripts; do any numeric derivation (polynomial
coefficients, error bounds) yourself, and let the check measure it.

## Method

1. Read the code you need.
2. **Before editing, state your hypothesis in one or two sentences:** what
   bounds this opcode's speed (e.g. a serial feedback recurrence, a
   transcendental call, memory bandwidth, branches, VM overhead) and the
   single change you will make to address it.
3. Make **one focused change**. Do not repeat an idea from the attempts table
   below unless you fix the reason it failed.
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T1017Z_gpt-5-6-sol_arith/check.sh`. It builds, checks the allowlist and bit-identity for
   every opcode, prints compiler vectoriser remarks for `cedar/include/cedar/opcodes/arithmetic.hpp`, and runs a
   short benchmark (~1 minute). The full verification afterwards also runs
   the hidden stimuli, all unit test suites, the zero-allocation trap, the
   opcode's DSP experiments and a longer benchmark.
5. Stop. Leave your changes in the working tree; do not commit.

**Your final message must end with exactly one line of this form:**

```
IDEA: <bottleneck> -> <the change you made> (<what the check showed>)
```

## Previous attempts in this run

| # | Verdict | Idea (the agent's own IDEA line) | Speedup vs previous |
|---|---|---|---|
| 1 | ACCEPTED | vector-width throughput, alias-versioning, and loop overhead -> fully unrolled aligned AVX2 kernels for add/sub/mul (all exact checks passed; 1.22–1.27x faster) | add 1.283x, sub 1.271x, mul 1.209x |
| 2 | ACCEPTED | AVX2 target-function call overhead -> always-inlined four-vector AVX2 assembly kernels with explicit vzeroupper (exact checks passed; mul 1.09x faster, add/sub unchanged within noise) | add 0.993x, sub 0.99x, mul 1.062x |


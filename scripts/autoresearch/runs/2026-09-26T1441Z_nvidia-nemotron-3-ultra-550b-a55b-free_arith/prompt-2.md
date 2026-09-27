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
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_add` | 14.47 | 10.86 | 1.33x |
| `op_sub` | 14.54 | 10.88 | 1.34x |
| `op_mul` | 14.47 | 11.11 | 1.30x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp b/cedar/include/cedar/opcodes/arithmetic.hpp
index 72deca0..73c9a73 100644
--- a/cedar/include/cedar/opcodes/arithmetic.hpp
+++ b/cedar/include/cedar/opcodes/arithmetic.hpp
@@ -3,10 +3,85 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline void add_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+#endif
+
+#if CEDAR_SIMD_NEON
+inline void add_neon(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        float32x4_t va = vld1q_f32(a + i);
+        float32x4_t vb = vld1q_f32(b + i);
+        vst1q_f32(out + i, vaddq_f32(va, vb));
+    }
+}
+
+inline void sub_neon(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        float32x4_t va = vld1q_f32(a + i);
+        float32x4_t vb = vld1q_f32(b + i);
+        vst1q_f32(out + i, vsubq_f32(va, vb));
+    }
+}
+
+inline void mul_neon(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        float32x4_t va = vld1q_f32(a + i);
+        float32x4_t vb = vld1q_f32(b + i);
+        vst1q_f32(out + i, vmulq_f32(va, vb));
+    }
+}
+#endif
+
+#if CEDAR_SIMD_WASM
+inline void add_wasm(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        v128_t va = wasm_v128_load(a + i);
+        v128_t vb = wasm_v128_load(b + i);
+        wasm_v128_store(out + i, wasm_f32x4_add(va, vb));
+    }
+}
+
+inline void sub_wasm(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        v128_t va = wasm_v128_load(a + i);
+        v128_t vb = wasm_v128_load(b + i);
+        wasm_v128_store(out + i, wasm_f32x4_sub(va, vb));
+    }
+}
+
+inline void mul_wasm(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        v128_t va = wasm_v128_load(a + i);
+        v128_t vb = wasm_v128_load(b + i);
+        wasm_v128_store(out + i, wasm_f32x4_mul(va, vb));
+    }
+}
+#endif
+}
+
 // ADD: out = in0 + in1
 [[gnu::always_inline]]
 inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
@@ -14,6 +89,15 @@ inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
+#endif
+#if CEDAR_SIMD_NEON
+    kernels::add_neon(a, b, out); return;
+#endif
+#if CEDAR_SIMD_WASM
+    kernels::add_wasm(a, b, out); return;
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] + b[i];
     }
@@ -26,6 +110,15 @@ inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::sub_avx2(a, b, out); return; }
+#endif
+#if CEDAR_SIMD_NEON
+    kernels::sub_neon(a, b, out); return;
+#endif
+#if CEDAR_SIMD_WASM
+    kernels::sub_wasm(a, b, out); return;
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] - b[i];
     }
@@ -38,6 +131,15 @@ inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
+#endif
+#if CEDAR_SIMD_NEON
+    kernels::mul_neon(a, b, out); return;
+#endif
+#if CEDAR_SIMD_WASM
+    kernels::mul_wasm(a, b, out); return;
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1441Z_nvidia-nemotron-3-ultra-550b-a55b-free_arith/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | Memory-bound scalar loops on x86-64 -> Added AVX2/NEON/WASM SIMD kernels with explicit vector intrinsics processing 8/4 floats per iteration, keeping the original scalar loop as exact fallback; all th | add 1.341x, sub 1.335x, mul 1.307x |


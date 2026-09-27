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
| `op_add` | 14.47 | 10.89 | 1.33x |
| `op_sub` | 14.54 | 11.13 | 1.31x |
| `op_mul` | 14.47 | 11.07 | 1.31x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp b/cedar/include/cedar/opcodes/arithmetic.hpp
index 72deca0..1df8310 100644
--- a/cedar/include/cedar/opcodes/arithmetic.hpp
+++ b/cedar/include/cedar/opcodes/arithmetic.hpp
@@ -3,8 +3,40 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline void add_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+#endif
+}  // namespace kernels
+
 namespace cedar {
 
 // ADD: out = in0 + in1
@@ -14,6 +46,12 @@ inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
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
@@ -26,6 +64,12 @@ inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
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
@@ -38,6 +82,12 @@ inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1518Z_cohere-north-mini-code-free_arith/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | Scalar loop over 128 samples limited ILP; added AVX2 kernels with 256‑bit vector loads/stores and conditional dispatch to preserve scalar fallback, achieving ~15% (add/sub) and ~11% (mul) speedup on t | add 1.337x, sub 1.307x, mul 1.313x |
| 2 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_add 1.018x (band 3.1%), op_sub 0.993x (band 2.6%), op_mul 0.993x | (no IDEA line) | — |
| 3 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_add 1.002x (band 2.7%), op_sub 0.989x (band 3.5%), op_mul 1.001x | (no IDEA line) | — |
| 4 | rejected: `empty patch` — no files changed since the last accepted state | Loop overhead in scalar fallback and lack of __restrict__ and unrolling in AVX2 kernels were slowing down op_add, op_sub, op_mul on the gate machine. The change adds __restrict__ qualifiers and fully  | — |
| 5 | rejected: `speed` — slower than baseline: op_mul 0.978x | (no IDEA line) | — |

Full verifier output for the latest rejection (attempt 5, gate `speed`):
```
slower than baseline: op_mul 0.978x
```

**The last 4 attempts were all rejected.** Try a structurally different approach from everything in the table above (different data layout, different loop structure, a different opcode in the list) rather than a variation of them.


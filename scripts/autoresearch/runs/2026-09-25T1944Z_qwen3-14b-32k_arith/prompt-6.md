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

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = a[i] * b[i];
    }
}
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_add` | 14.47 | 11.14 | 1.30x |
| `op_sub` | 14.54 | 11.02 | 1.32x |
| `op_mul` | 14.47 | 11.05 | 1.31x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp b/cedar/include/cedar/opcodes/arithmetic.hpp
index 72deca0..04c7439 100644
--- a/cedar/include/cedar/opcodes/arithmetic.hpp
+++ b/cedar/include/cedar/opcodes/arithmetic.hpp
@@ -3,10 +3,35 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline void add_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
+        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+}
+#endif
+
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
+        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+}
+#endif
+
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
+        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+}
+#endif
+
+}  // namespace kernels
+
 // ADD: out = in0 + in1
 [[gnu::always_inline]]
 inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
@@ -14,6 +39,10 @@ inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] + b[i];
     }
@@ -26,6 +55,10 @@ inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::sub_avx2(a, b, out); return; }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] - b[i];
     }
@@ -38,6 +71,10 @@ inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] * b[i];
     }
@@ -78,4 +115,4 @@ inline void op_neg(ExecutionContext& ctx, const Instruction& inst) {
     }
 }
 
-}  // namespace cedar
+}  // namespace cedar
\ No newline at end of file
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1944Z_qwen3-14b-32k_arith/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | rejected: `build` — /home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1944Z_qwen3-14b-32k_arith/wt/cedar/src/dsp/simd.cpp:1: | (no IDEA line) | — |
| 2 | rejected: `build` — ../../../cedar/include/cedar/opcodes/arithmetic.hpp:26:2: error: stray ‘#’ in program | (no IDEA line) | — |
| 3 | rejected: `build` — ../../../cedar/include/cedar/opcodes/arithmetic.hpp:21:9: error: ‘kernels’ has not been declared | (no IDEA line) | — |
| 4 | rejected: `build` — ../../../cedar/src/vm/vm.cpp:1470:13: error: ‘op_add’ was not declared in this scope; did you mean ‘op_dc’? | Memory bandwidth and scalar loop overhead -> AVX2 vectorization with inlined kernels (simd::has check and kernel calls inside #if CEDAR_SIMD_X86 block, kernels defined before opcode functions) (check  | — |
| 5 | ACCEPTED | Scalar loop overhead and lack of vectorization -> AVX2 vectorization with inlined kernels (simd::has check and kernel calls inside #if CEDAR_SIMD_X86 block, kernels defined before opcode functions) (c | add 1.307x, sub 1.322x, mul 1.318x |

Full verifier output for the latest rejection (attempt 4, gate `build`):
```
../../../cedar/src/vm/vm.cpp:1470:13: error: ‘op_add’ was not declared in this scope; did you mean ‘op_dc’?
../../../cedar/src/vm/vm.cpp:1474:13: error: ‘op_sub’ was not declared in this scope
../../../cedar/src/vm/vm.cpp:1478:13: error: ‘op_mul’ was not declared in this scope; did you mean ‘op_min’?
../../../cedar/src/vm/vm.cpp:1482:13: error: ‘op_div’ was not declared in this scope; did you mean ‘op_min’?
../../../cedar/src/vm/vm.cpp:1486:13: error: ‘op_pow’ was not declared in this scope; did you mean ‘op_pan’?
../../../cedar/src/vm/vm.cpp:1490:13: error: ‘op_neg’ was not declared in this scope; did you mean ‘op_log’?
gmake[3]: *** [cedar/CMakeFiles/cedar.dir/build.make:90: cedar/CMakeFiles/cedar.dir/src/vm/vm.cpp.o] Error 1
gmake[2]: *** [CMakeFiles/Makefile2:415: cedar/CMakeFiles/cedar.dir/all] Error 2
gmake[1]: *** [CMakeFiles/Makefile2:474: cedar/benchmarks/CMakeFiles/cedar_bench.dir/rule] Error 2
gmake: *** [Makefile:258: cedar_bench] Error 2
```


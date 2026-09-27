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
| `op_add` | 14.47 | 12.45 | 1.16x |
| `op_sub` | 14.54 | 12.45 | 1.17x |
| `op_mul` | 14.47 | 12.36 | 1.17x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp b/cedar/include/cedar/opcodes/arithmetic.hpp
index 72deca0..8f4ce71 100644
--- a/cedar/include/cedar/opcodes/arithmetic.hpp
+++ b/cedar/include/cedar/opcodes/arithmetic.hpp
@@ -3,10 +3,46 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline void add_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 16) {
+        __m256 x0 = _mm256_load_ps(a + i);
+        __m256 x1 = _mm256_load_ps(a + i + 8);
+        __m256 y0 = _mm256_load_ps(b + i);
+        __m256 y1 = _mm256_load_ps(b + i + 8);
+        _mm256_store_ps(out + i, _mm256_add_ps(x0, y0));
+        _mm256_store_ps(out + i + 8, _mm256_add_ps(x1, y1));
+    }
+}
+CEDAR_TARGET_AVX2 inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 16) {
+        __m256 x0 = _mm256_load_ps(a + i);
+        __m256 x1 = _mm256_load_ps(a + i + 8);
+        __m256 y0 = _mm256_load_ps(b + i);
+        __m256 y1 = _mm256_load_ps(b + i + 8);
+        _mm256_store_ps(out + i, _mm256_sub_ps(x0, y0));
+        _mm256_store_ps(out + i + 8, _mm256_sub_ps(x1, y1));
+    }
+}
+CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 16) {
+        __m256 x0 = _mm256_load_ps(a + i);
+        __m256 x1 = _mm256_load_ps(a + i + 8);
+        __m256 y0 = _mm256_load_ps(b + i);
+        __m256 y1 = _mm256_load_ps(b + i + 8);
+        _mm256_store_ps(out + i, _mm256_mul_ps(x0, y0));
+        _mm256_store_ps(out + i + 8, _mm256_mul_ps(x1, y1));
+    }
+}
+#endif
+}  // namespace kernels
+
 // ADD: out = in0 + in1
 [[gnu::always_inline]]
 inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
@@ -14,6 +50,9 @@ inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] + b[i];
     }
@@ -26,6 +65,9 @@ inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::sub_avx2(a, b, out); return; }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] - b[i];
     }
@@ -38,6 +80,9 @@ inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1940Z_sonnet_arith/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | element-wise loop issued 4 floats per instruction (GCC already SSE2-vectorised it) -> AVX2 8-wide load/op/store kernels unrolled 2x for add/sub/mul, runtime-dispatched with scalar fallback kept (check | add 1.352x, sub 1.375x, mul 1.364x |
| 2 | rejected: `empty patch` — no files changed since the last accepted state | store-miss latency on strided output buffers -> prefetch all 8 output cache lines at kernel entry (bit-identical, but add got slower in both runs (0.79x, 0.64x), sub and mul were within noise, so I re | — |
| 3 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_add 1.065x (band 32.3%), op_sub 0.897x (band 73.0%), op_mul 0.96 | front-end/loop overhead around an already port-bound AVX2 kernel -> fully unrolled the add/sub/mul AVX2 kernels with constant offsets so loads can fold into the ops (bit-identical, but the benchmark w | — |
| 4 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_add 1.031x (band 16.9%), op_sub 1.217x (band 29.1%), op_mul 1.08 | L1↔L2 read-for-ownership traffic on cold output lines (kernel arithmetic is only ~10 of ~36 cycles) -> software-pipelined output prefetch (2 lines primed at entry, then line k+2 during line k) in the  | — |
| 5 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_add 0.875x (band 18.9%), op_sub 1.024x (band 36.8%), op_mul 1.01 | L1↔L2 read-for-ownership and eviction traffic on aliased output buffers -> replaced the 2x-unrolled kernels with one-vector-per-iteration load/op/store loops (a 4-line batched-store variant was 0.82–0 | — |
| 6 | rejected: `empty patch` — no files changed since the last accepted state | VM buffer-resolution overhead (3 bounds-checked BufferPool::get() calls per op) -> resolve out/in0/in1 with a single slab lookup before the AVX2 kernel (bit-identical, but add 0.95x/0.65x and mul 0.81 | — |
| 7 | rejected: `empty patch` — no files changed since the last accepted state | L1↔L2 fill/writeback traffic on cold output lines -> demand-load all 8 output lines at kernel entry so the fills overlap compute (bit-identical, but add 0.81x and sub 0.87x, mul 1.14x within noise; re | — |

Full verifier output for the latest rejection (attempt 7, gate `empty patch`):
```
no files changed since the last accepted state
```

**The last 6 attempts were all rejected.** Try a structurally different approach from everything in the table above (different data layout, different loop structure, a different opcode in the list) rather than a variation of them.


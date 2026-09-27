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
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_add` | 14.47 | 12.49 | 1.16x |
| `op_sub` | 14.54 | 12.85 | 1.13x |
| `op_mul` | 14.47 | 12.18 | 1.19x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp b/cedar/include/cedar/opcodes/arithmetic.hpp
index 72deca0..e3f4447 100644
--- a/cedar/include/cedar/opcodes/arithmetic.hpp
+++ b/cedar/include/cedar/opcodes/arithmetic.hpp
@@ -3,10 +3,128 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 
 namespace cedar {
 
+// --- SIMD kernels: ADD / SUB / MUL ----------------------------------------
+//
+// Element-wise IEEE-754 add/sub/mul over one block, widened from the scalar
+// bodies below (which stay as oracle + fallback). add/sub/mul are single
+// rounded operations per element, so 8-lane AVX2 / 4-lane SSE2-NEON-WASM
+// code is bit-identical to the scalar loop for *every* input — denormals,
+// infinities, NaNs and signed zeros included. No FMA (would change
+// rounding), no FTZ/DAZ (MXCSR is left untouched).
+//
+// Kernels may assume 32-byte aligned buffers (BufferPool slabs are
+// alignas(32)) and BLOCK_SIZE is a compile-time multiple of the lane count,
+// so each loop fully unrolls. No allocation, lock or syscall; the ISA check
+// in the opcode reads a global written once at static init.
+namespace kernels {
+
+#if CEDAR_SIMD_X86
+// AVX2: 8 lanes, aligned loads/stores.
+CEDAR_TARGET_AVX2 inline void add_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+    }
+}
+
+// SSE2: 4 lanes, for x86-64 hosts without AVX2 (SSE2 is baseline).
+inline void add_sse2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 16);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 16);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 16);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        _mm_store_ps(out + i, _mm_add_ps(_mm_load_ps(a + i), _mm_load_ps(b + i)));
+    }
+}
+
+inline void sub_sse2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 16);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 16);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 16);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        _mm_store_ps(out + i, _mm_sub_ps(_mm_load_ps(a + i), _mm_load_ps(b + i)));
+    }
+}
+
+inline void mul_sse2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 16);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 16);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 16);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        _mm_store_ps(out + i, _mm_mul_ps(_mm_load_ps(a + i), _mm_load_ps(b + i)));
+    }
+}
+#endif  // CEDAR_SIMD_X86
+
+#if CEDAR_SIMD_NEON
+// aarch64: NEON is baseline, 4 lanes.
+inline void add_neon(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        vst1q_f32(out + i, vaddq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
+    }
+}
+
+inline void sub_neon(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        vst1q_f32(out + i, vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
+    }
+}
+
+inline void mul_neon(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        vst1q_f32(out + i, vmulq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
+    }
+}
+#endif  // CEDAR_SIMD_NEON
+
+#if CEDAR_SIMD_WASM
+// WASM SIMD128: 4 lanes.
+inline void add_wasm(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        wasm_v128_store(out + i, wasm_f32x4_add(wasm_v128_load(a + i), wasm_v128_load(b + i)));
+    }
+}
+
+inline void sub_wasm(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        wasm_v128_store(out + i, wasm_f32x4_sub(wasm_v128_load(a + i), wasm_v128_load(b + i)));
+    }
+}
+
+inline void mul_wasm(const float* a, const float* b, float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
+        wasm_v128_store(out + i, wasm_f32x4_mul(wasm_v128_load(a + i), wasm_v128_load(b + i)));
+    }
+}
+#endif  // CEDAR_SIMD_WASM
+
+}  // namespace kernels
+
 // ADD: out = in0 + in1
 [[gnu::always_inline]]
 inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
@@ -14,6 +132,14 @@ inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
+    if (simd::has(simd::Isa::Sse2)) { kernels::add_sse2(a, b, out); return; }
+#elif CEDAR_SIMD_NEON
+    if (simd::has(simd::Isa::Neon)) { kernels::add_neon(a, b, out); return; }
+#elif CEDAR_SIMD_WASM
+    if (simd::has(simd::Isa::Wasm128)) { kernels::add_wasm(a, b, out); return; }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] + b[i];
     }
@@ -26,6 +152,14 @@ inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::sub_avx2(a, b, out); return; }
+    if (simd::has(simd::Isa::Sse2)) { kernels::sub_sse2(a, b, out); return; }
+#elif CEDAR_SIMD_NEON
+    if (simd::has(simd::Isa::Neon)) { kernels::sub_neon(a, b, out); return; }
+#elif CEDAR_SIMD_WASM
+    if (simd::has(simd::Isa::Wasm128)) { kernels::sub_wasm(a, b, out); return; }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] - b[i];
     }
@@ -38,6 +172,14 @@ inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
+    if (simd::has(simd::Isa::Sse2)) { kernels::mul_sse2(a, b, out); return; }
+#elif CEDAR_SIMD_NEON
+    if (simd::has(simd::Isa::Neon)) { kernels::mul_neon(a, b, out); return; }
+#elif CEDAR_SIMD_WASM
+    if (simd::has(simd::Isa::Wasm128)) { kernels::mul_wasm(a, b, out); return; }
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1941Z_z-ai-glm-5-3_arith/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | per-element load/store µop count (baseline SSE2 code with a runtime alias version check, no -mavx2) -> explicit 32-byte-aligned 8-lane AVX2 kernels for add/sub/mul behind the existing simd::has() disp | add 1.371x, sub 1.398x, mul 1.338x |
| 2 | rejected: `backend error` — wall-clock cap of 1800 s hit | (no IDEA line) | — |
| 3 | rejected: `backend error` — budget exhausted mid-iteration | (no IDEA line) | — |
| 4 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_add 1.009x (band 9.3%), op_sub 1.069x (band 11.9%), op_mul 0.957 | VM dispatch overhead (three guarded BufferPool::get() calls per opcode: bounds check, slab load, null check, cold diagnostics) -> kernels::buf_fast() resolving slab/stride arithmetic directly on the S | — |

Full verifier output for the latest rejection (attempt 4, gate `speed`):
```
no opcode improved by >= 5 % outside the noise band: op_add 1.009x (band 9.3%), op_sub 1.069x (band 11.9%), op_mul 0.957x (band 8.9%)
```

**The last 3 attempts were all rejected.** Try a structurally different approach from everything in the table above (different data layout, different loop structure, a different opcode in the list) rather than a variation of them.


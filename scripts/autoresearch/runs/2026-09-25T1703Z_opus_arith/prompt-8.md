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
| `op_add` | 14.47 | 10.27 | 1.41x |
| `op_sub` | 14.54 | 10.35 | 1.40x |
| `op_mul` | 14.47 | 10.37 | 1.40x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp b/cedar/include/cedar/opcodes/arithmetic.hpp
index 72deca0..ab01f6d 100644
--- a/cedar/include/cedar/opcodes/arithmetic.hpp
+++ b/cedar/include/cedar/opcodes/arithmetic.hpp
@@ -3,10 +3,55 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+// Element-wise binary ops. Each 8-lane vector is fully loaded before being
+// stored, so in-place use (out == a or out == b) is safe.
+//
+// The output block is 8 cache lines; when it is not L1-resident each store
+// needs an RFO, and the store buffer would otherwise wait on them one line
+// at a time. prefetchw on all 8 lines up front overlaps those RFOs with the
+// loads and arithmetic. (prfchw ships on every AVX2 CPU; it's a hint only.)
+#define CEDAR_TARGET_AVX2_PRFCHW [[gnu::target("avx2,prfchw")]]
+CEDAR_TARGET_AVX2_PRFCHW inline void prefetch_block_for_write(float* out) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 16) _m_prefetchw(out + i);
+}
+
+CEDAR_TARGET_AVX2_PRFCHW inline void add_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    prefetch_block_for_write(out);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
+        _mm256_store_ps(out + i, _mm256_add_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+}
+
+CEDAR_TARGET_AVX2_PRFCHW inline void sub_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    prefetch_block_for_write(out);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
+        _mm256_store_ps(out + i, _mm256_sub_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+}
+
+CEDAR_TARGET_AVX2_PRFCHW inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(a, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(b, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out, 32);
+    prefetch_block_for_write(out);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
+        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
+}
+#undef CEDAR_TARGET_AVX2_PRFCHW
+#endif
+}  // namespace kernels
+
 // ADD: out = in0 + in1
 [[gnu::always_inline]]
 inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
@@ -14,6 +59,9 @@ inline void op_add(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::add_avx2(a, b, out); return; }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] + b[i];
     }
@@ -26,6 +74,9 @@ inline void op_sub(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::sub_avx2(a, b, out); return; }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] - b[i];
     }
@@ -38,6 +89,9 @@ inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
     const float* a = ctx.buffers->get(inst.inputs[0]);
     const float* b = ctx.buffers->get(inst.inputs[1]);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         out[i] = a[i] * b[i];
     }
diff --git a/cedar/include/cedar/opcodes/arithmetic.hpp.new b/cedar/include/cedar/opcodes/arithmetic.hpp.new
new file mode 100644
index 0000000..e69de29
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1703Z_opus_arith/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | SSE-width auto-vectorised loops behind a runtime alias check, over a 128-float block -> AVX2 kernels (aligned 256-bit load/op/store, no FMA) for add/sub/mul, called when AVX2 is available, scalar fall | add 1.31x, sub 1.311x, mul 1.306x |
| 2 | ACCEPTED | output-block stores missing L1 (outputs 1 KB apart crowd into half the L1 sets) so each store waits on a fetch from L2 -> prefetchw all 8 output cache lines at kernel entry in add/sub/mul AVX2 kernels | add 1.059x, sub 1.083x, mul 1.069x |
| 3 | rejected: `empty patch` — no files changed since the last accepted state | output-block stores waiting on RFOs from L2 -> tried loads-before-stores (with and without full unroll), descending order and half-rate prefetchw in add/sub/mul AVX2 kernels, then reverted to the acce | — |
| 4 | rejected: `empty patch` — no files changed since the last accepted state | output stores still wait on RFOs from L2 because the in-kernel prefetchw is issued too late -> prefetchw the output block of the next instruction (and, in a second try, two ahead and before the kernel | — |
| 5 | rejected: `empty patch` — no files changed since the last accepted state | call/prologue/vzeroupper overhead of a non-inlinable target("avx2") kernel -> rewrote add/sub/mul AVX2 kernels as inline asm (same prefetchw + vmovaps/vop/vmovaps sequence) so they inline into the VM  | — |
| 6 | rejected: `empty patch` — no files changed since the last accepted state | output blocks thrash half the L1 sets (1 KB stride), so each block costs 8 L2 line fills plus 8 dirty write-backs -> tried adding a prefetchw for a 9th line (32-byte slab offset) and a full unroll of  | — |
| 7 | rejected: `empty patch` — no files changed since the last accepted state | output lines thrash L1 (16 lines per set over 12 ways, from the 1 KB output stride), so each block costs L1↔L2 traffic -> swapped the in-kernel prefetchw for prefetchnta so most output lines might sta | — |

Full verifier output for the latest rejection (attempt 7, gate `empty patch`):
```
no files changed since the last accepted state
```

**The last 5 attempts were all rejected.** Try a structurally different approach from everything in the table above (different data layout, different loop structure, a different opcode in the list) rather than a variation of them.


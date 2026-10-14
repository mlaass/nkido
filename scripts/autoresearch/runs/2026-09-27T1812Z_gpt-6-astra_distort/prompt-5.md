You are optimising Cedar DSP opcodes with SIMD. Cedar is the real-time audio
engine of nkido: a bytecode VM that processes blocks of 128 float samples at
48 kHz, zero allocations on the audio path. The repository is your current
working directory.

## Task

Make these opcodes faster on **x86-64 AVX2** (the gate machine):

- `op_distort_tanh`
- `op_distort_soft`

They live in `cedar/include/cedar/opcodes/distortion.hpp`. Current source of the opcode bodies:

```cpp
[[gnu::always_inline]]
inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* drive = ctx.buffers->get(inst.inputs[1]);
    const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
    const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::distort_tanh_avx2(input, input_r, drive, dry_level, wet_level, out_l, out_r);
        return;
    }
#endif

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float d = std::max(0.1f, drive[i]);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);
        float x_l = input[i];
        float x_r = stereo_in ? input_r[i] : x_l;
        out_l[i] = drywet::mix(x_l, std::tanh(x_l * d), dry, wet);
        out_r[i] = drywet::mix(x_r, std::tanh(x_r * d), dry, wet);
    }
}

[[gnu::always_inline]]
inline void op_distort_soft(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* threshold = ctx.buffers->get(inst.inputs[1]);
    const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
    const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::distort_soft_avx2(input, input_r, threshold, dry_level, wet_level, out_l, out_r);
        return;
    }
#endif

    auto soft = [](float x) {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    };

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float t = std::clamp(threshold[i], 0.1f, 2.0f);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);
        float x_l = input[i];
        float x_r = stereo_in ? input_r[i] : x_l;
        out_l[i] = drywet::mix(x_l, soft(x_l / t) * t, dry, wet);
        out_r[i] = drywet::mix(x_r, soft(x_r / t) * t, dry, wet);
    }
}
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_distort_tanh` | 974.44 | 129.15 | 7.55x |
| `op_distort_soft` | 257.33 | 93.05 | 2.77x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/distortion.hpp b/cedar/include/cedar/opcodes/distortion.hpp
index 4a294b4..3251b7f 100644
--- a/cedar/include/cedar/opcodes/distortion.hpp
+++ b/cedar/include/cedar/opcodes/distortion.hpp
@@ -1,5 +1,7 @@
 #pragma once
 
+#include "../dsp/simd.hpp"
+
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
@@ -20,6 +22,58 @@ namespace cedar {
 // Classic warm saturation using hyperbolic tangent.
 // Higher drive values push the signal into saturation more aggressively.
 
+#if CEDAR_SIMD_X86
+namespace kernels {
+// simd: approx std::tanh via a [7/6] Pade approximant on [-2.25, 2.25].
+// Two double-angle steps recover tanh(x); beyond |x|=9 the tail is < 4e-8.
+CEDAR_TARGET_AVX2 inline __m256 distort_tanh_curve_avx2(__m256 x) noexcept {
+    x = _mm256_max_ps(_mm256_set1_ps(-9.0f),
+        _mm256_min_ps(_mm256_set1_ps(9.0f), x));
+    x = _mm256_mul_ps(x, _mm256_set1_ps(0.25f));
+    const __m256 z = _mm256_mul_ps(x, x);
+    __m256 p = _mm256_add_ps(_mm256_set1_ps(378.0f), z);
+    p = _mm256_add_ps(_mm256_set1_ps(17325.0f), _mm256_mul_ps(z, p));
+    p = _mm256_add_ps(_mm256_set1_ps(135135.0f), _mm256_mul_ps(z, p));
+    __m256 q = _mm256_add_ps(_mm256_set1_ps(3150.0f),
+        _mm256_mul_ps(_mm256_set1_ps(28.0f), z));
+    q = _mm256_add_ps(_mm256_set1_ps(62370.0f), _mm256_mul_ps(z, q));
+    q = _mm256_add_ps(_mm256_set1_ps(135135.0f), _mm256_mul_ps(z, q));
+    __m256 y = _mm256_div_ps(_mm256_mul_ps(x, p), q);
+    y = _mm256_div_ps(_mm256_add_ps(y, y),
+        _mm256_add_ps(_mm256_set1_ps(1.0f), _mm256_mul_ps(y, y)));
+    return _mm256_div_ps(_mm256_add_ps(y, y),
+        _mm256_add_ps(_mm256_set1_ps(1.0f), _mm256_mul_ps(y, y)));
+}
+
+CEDAR_TARGET_AVX2 inline void distort_tanh_avx2(
+    const float* input, const float* input_r, const float* drive,
+    const float* dry_level, const float* wet_level, float* out_l, float* out_r) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        // std::max(0.1f, drive) returns 0.1f for a NaN drive.
+        const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i),
+            _mm256_set1_ps(0.1f));
+        const __m256 y_l = distort_tanh_curve_avx2(
+            _mm256_mul_ps(_mm256_load_ps(input + i), d));
+        const __m256 y_r = input_r ? distort_tanh_curve_avx2(
+            _mm256_mul_ps(_mm256_load_ps(input_r + i), d)) : y_l;
+        alignas(32) float shaped_l[8];
+        alignas(32) float shaped_r[8];
+        _mm256_store_ps(shaped_l, y_l);
+        _mm256_store_ps(shaped_r, y_r);
+        for (std::size_t lane = 0; lane < 8; ++lane) {
+            const std::size_t j = i + lane;
+            const float dry = drywet::coeff(dry_level, j, 0.0f);
+            const float wet = drywet::coeff(wet_level, j, 1.0f);
+            const float left = input[j];
+            const float right = input_r ? input_r[j] : left;
+            out_l[j] = drywet::mix(left, shaped_l[lane], dry, wet);
+            out_r[j] = drywet::mix(right, shaped_r[lane], dry, wet);
+        }
+    }
+}
+} // namespace kernels
+#endif
+
 [[gnu::always_inline]]
 inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
     float* out_l = ctx.buffers->get(inst.out_buffer);
@@ -33,6 +87,13 @@ inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
     const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
     const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::distort_tanh_avx2(input, input_r, drive, dry_level, wet_level, out_l, out_r);
+        return;
+    }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         float d = std::max(0.1f, drive[i]);
         float dry = drywet::coeff(dry_level, i, 0.0f);
@@ -53,6 +114,50 @@ inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
 // Smooth polynomial soft clipper that rounds off peaks gradually.
 // Uses cubic polynomial for continuous first derivative.
 
+#if CEDAR_SIMD_X86
+namespace kernels {
+CEDAR_TARGET_AVX2 inline __m256 distort_soft_curve_avx2(__m256 x) noexcept {
+    const __m256 x2 = _mm256_mul_ps(x, x);
+    const __m256 numerator = _mm256_mul_ps(x, _mm256_add_ps(_mm256_set1_ps(27.0f), x2));
+    const __m256 denominator = _mm256_add_ps(_mm256_set1_ps(27.0f),
+        _mm256_mul_ps(_mm256_set1_ps(9.0f), x2));
+    __m256 y = _mm256_div_ps(numerator, denominator);
+    y = _mm256_blendv_ps(y, _mm256_set1_ps(1.0f),
+        _mm256_cmp_ps(x, _mm256_set1_ps(3.0f), _CMP_GT_OQ));
+    return _mm256_blendv_ps(y, _mm256_set1_ps(-1.0f),
+        _mm256_cmp_ps(x, _mm256_set1_ps(-3.0f), _CMP_LT_OQ));
+}
+
+CEDAR_TARGET_AVX2 inline void distort_soft_avx2(
+    const float* input, const float* input_r, const float* threshold,
+    const float* dry_level, const float* wet_level, float* out_l, float* out_r) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        // Operand order preserves std::clamp's handling of a NaN threshold.
+        const __m256 t = _mm256_min_ps(_mm256_set1_ps(2.0f),
+            _mm256_max_ps(_mm256_set1_ps(0.1f), _mm256_load_ps(threshold + i)));
+        const __m256 x_l = _mm256_load_ps(input + i);
+        const __m256 y_l = _mm256_mul_ps(
+            distort_soft_curve_avx2(_mm256_div_ps(x_l, t)), t);
+        const __m256 y_r = input_r ? _mm256_mul_ps(distort_soft_curve_avx2(
+            _mm256_div_ps(_mm256_load_ps(input_r + i), t)), t) : y_l;
+        alignas(32) float shaped_l[8];
+        alignas(32) float shaped_r[8];
+        _mm256_store_ps(shaped_l, y_l);
+        _mm256_store_ps(shaped_r, y_r);
+        for (std::size_t lane = 0; lane < 8; ++lane) {
+            const std::size_t j = i + lane;
+            const float dry = drywet::coeff(dry_level, j, 0.0f);
+            const float wet = drywet::coeff(wet_level, j, 1.0f);
+            const float left = input[j];
+            const float right = input_r ? input_r[j] : left;
+            out_l[j] = drywet::mix(left, shaped_l[lane], dry, wet);
+            out_r[j] = drywet::mix(right, shaped_r[lane], dry, wet);
+        }
+    }
+}
+} // namespace kernels
+#endif
+
 [[gnu::always_inline]]
 inline void op_distort_soft(ExecutionContext& ctx, const Instruction& inst) {
     float* out_l = ctx.buffers->get(inst.out_buffer);
@@ -66,6 +171,13 @@ inline void op_distort_soft(ExecutionContext& ctx, const Instruction& inst) {
     const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
     const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::distort_soft_avx2(input, input_r, threshold, dry_level, wet_level, out_l, out_r);
+        return;
+    }
+#endif
+
     auto soft = [](float x) {
         if (x > 3.0f) return 1.0f;
         if (x < -3.0f) return -1.0f;
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
   Exception: op_distort_tanh call `tanh`/`tan`; for these only, an approximation is allowed if (a) you mark it with a `// simd: approx` comment naming the replaced function, and (b) its peak error stays at or below -100 dBFS (about 1e-5 absolute) on every stimulus.
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T1812Z_gpt-6-astra_distort/check.sh`. It builds, checks the allowlist and bit-identity for
   every opcode, prints compiler vectoriser remarks for `cedar/include/cedar/opcodes/distortion.hpp`, and runs a
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
| 1 | rejected: `empty patch` — no files changed since the last accepted state | scalar distortion arithmetic -> pending source inspection; no change made (check not run) | — |
| 2 | ACCEPTED | scalar divisions and clipping branches -> eight-lane AVX2 soft-distortion arithmetic (check passed exact equality; soft 2.77× faster, tanh unchanged) | distort_tanh 1.002x, distort_soft 2.759x |
| 3 | ACCEPTED | per-sample transcendental calls -> eight-lane AVX2 rational tanh approximation (check passed; tanh 978.4→129.21 ns/block, soft 93.81→93.24 ns/block) | distort_tanh 7.575x, distort_soft 1.007x |
| 4 | rejected: `speed` — slower than baseline: op_distort_tanh 0.810x | dependent vector divisions -> Newton-refined reciprocals in tanh (check passed accuracy; tanh 129.2→159.52 ns/block, soft 93.14→93.57; performance regression) | — |

Full verifier output for the latest rejection (attempt 4, gate `speed`):
```
slower than baseline: op_distort_tanh 0.810x
```


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

    auto soft = [](float x) {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    };

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::distort_soft_avx2(input, input_r, threshold, dry_level, wet_level, out_l, out_r);
        return;
    }
#endif

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
| `op_distort_tanh` | 974.44 | 47.79 | 20.39x |
| `op_distort_soft` | 257.33 | 42.95 | 5.99x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/distortion.hpp b/cedar/include/cedar/opcodes/distortion.hpp
index 4a294b4..d7f7b56 100644
--- a/cedar/include/cedar/opcodes/distortion.hpp
+++ b/cedar/include/cedar/opcodes/distortion.hpp
@@ -6,11 +6,105 @@
 #include "dsp_state.hpp"
 #include "dsp_utils.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+// simd: approx tanh — tanh(y) = 1 - 2 / (1 + e^(2y)) on y clamped to +-9.
+// e^(2y) = 2^n * 2^f with n = round(2y*log2e) via the 1.5*2^23 magic add (the
+// low mantissa bits of the sum are n + 127, shifted straight into the exponent)
+// and 2^f, |f| <= 0.5, from a Chebyshev-economised degree-4 Taylor polynomial
+// (relative error < 8e-6). The absolute tanh error is at most half the relative
+// error of e^(2y), so < 4e-6 everywhere; the formula is valid for both signs.
+CEDAR_TARGET_AVX2 inline __m256 tanh_approx_avx2(__m256 y) noexcept {
+    // min/max with the constant first return y when y is NaN, so NaN propagates
+    const __m256 x = _mm256_max_ps(_mm256_set1_ps(-9.0f),
+                                   _mm256_min_ps(_mm256_set1_ps(9.0f), y));
+    const __m256 t = _mm256_mul_ps(x, _mm256_set1_ps(2.88539008177792681f));  // 2*log2(e)
+    const __m256 magic = _mm256_set1_ps(12583039.0f);  // 1.5 * 2^23 + 127
+    const __m256 s = _mm256_add_ps(t, magic);
+    const __m256 f = _mm256_sub_ps(t, _mm256_sub_ps(s, magic));
+    __m256 p = _mm256_set1_ps(9.61812910762847716e-03f);
+    p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(5.59208190e-02f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(2.40226506959100712e-01f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(6.93121138e-01f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, f), _mm256_set1_ps(1.0f));
+    const __m256 pow2n = _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_castps_si256(s), 23));
+    const __m256 e = _mm256_mul_ps(p, pow2n);
+    const __m256 one = _mm256_set1_ps(1.0f);
+    return _mm256_sub_ps(one, _mm256_div_ps(_mm256_set1_ps(2.0f), _mm256_add_ps(one, e)));
+}
+
+CEDAR_TARGET_AVX2 inline void distort_tanh_avx2(const float* in_l, const float* in_r,
+                                                const float* drive, const float* dry_level,
+                                                const float* wet_level, float* out_l,
+                                                float* out_r) noexcept {
+    const __m256 min_drive = _mm256_set1_ps(0.1f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        // std::max(0.1f, d) == (0.1 < d) ? d : 0.1
+        const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i), min_drive);
+        const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i) : _mm256_setzero_ps();
+        const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : _mm256_set1_ps(1.0f);
+        const __m256 xl = _mm256_load_ps(in_l + i);
+        const __m256 yl = _mm256_add_ps(_mm256_mul_ps(xl, dry),
+                                        _mm256_mul_ps(tanh_approx_avx2(_mm256_mul_ps(xl, d)), wet));
+        _mm256_store_ps(out_l + i, yl);
+        if (in_r) {
+            const __m256 xr = _mm256_load_ps(in_r + i);
+            _mm256_store_ps(out_r + i,
+                            _mm256_add_ps(_mm256_mul_ps(xr, dry),
+                                          _mm256_mul_ps(tanh_approx_avx2(_mm256_mul_ps(xr, d)), wet)));
+        } else {
+            _mm256_store_ps(out_r + i, yl);
+        }
+    }
+}
+
+// Exact vector form of the scalar soft clipper: soft(x / t) * t.
+CEDAR_TARGET_AVX2 inline __m256 soft_clip_avx2(__m256 x, __m256 t) noexcept {
+    const __m256 u = _mm256_div_ps(x, t);
+    const __m256 u2 = _mm256_mul_ps(u, u);
+    const __m256 c27 = _mm256_set1_ps(27.0f);
+    __m256 v = _mm256_div_ps(_mm256_mul_ps(u, _mm256_add_ps(c27, u2)),
+                             _mm256_add_ps(c27, _mm256_mul_ps(_mm256_set1_ps(9.0f), u2)));
+    v = _mm256_blendv_ps(v, _mm256_set1_ps(-1.0f),
+                         _mm256_cmp_ps(u, _mm256_set1_ps(-3.0f), _CMP_LT_OQ));
+    v = _mm256_blendv_ps(v, _mm256_set1_ps(1.0f),
+                         _mm256_cmp_ps(u, _mm256_set1_ps(3.0f), _CMP_GT_OQ));
+    return _mm256_mul_ps(v, t);
+}
+
+CEDAR_TARGET_AVX2 inline void distort_soft_avx2(const float* in_l, const float* in_r,
+                                                const float* threshold, const float* dry_level,
+                                                const float* wet_level, float* out_l,
+                                                float* out_r) noexcept {
+    const __m256 lo = _mm256_set1_ps(0.1f);
+    const __m256 hi = _mm256_set1_ps(2.0f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        // std::clamp(v, lo, hi): (v < lo) ? lo : (hi < v) ? hi : v
+        const __m256 t = _mm256_min_ps(hi, _mm256_max_ps(lo, _mm256_load_ps(threshold + i)));
+        const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i) : _mm256_setzero_ps();
+        const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : _mm256_set1_ps(1.0f);
+        const __m256 xl = _mm256_load_ps(in_l + i);
+        const __m256 yl = _mm256_add_ps(_mm256_mul_ps(xl, dry),
+                                        _mm256_mul_ps(soft_clip_avx2(xl, t), wet));
+        _mm256_store_ps(out_l + i, yl);
+        if (in_r) {
+            const __m256 xr = _mm256_load_ps(in_r + i);
+            _mm256_store_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xr, dry),
+                                                     _mm256_mul_ps(soft_clip_avx2(xr, t), wet)));
+        } else {
+            _mm256_store_ps(out_r + i, yl);
+        }
+    }
+}
+#endif
+}  // namespace kernels
+
 // ============================================================================
 // DISTORT_TANH: Tanh Saturation
 // ============================================================================
@@ -33,6 +127,13 @@ inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
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
@@ -73,6 +174,13 @@ inline void op_distort_soft(ExecutionContext& ctx, const Instruction& inst) {
         return x * (27.0f + x2) / (27.0f + 9.0f * x2);
     };
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::distort_soft_avx2(input, input_r, threshold, dry_level, wet_level, out_l, out_r);
+        return;
+    }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         float t = std::clamp(threshold[i], 0.1f, 2.0f);
         float dry = drywet::coeff(dry_level, i, 0.0f);
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1739Z_opus_distort/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | scalar libm tanh call / branchy per-sample soft clip -> AVX2 kernels: exp-based tanh approx (Cephes expf poly + small-/y/ Taylor, // simd: approx tanh) and exact branchless blendv soft clipper, mono c | distort_tanh 10.544x, distort_soft 6.402x |
| 2 | ACCEPTED | exp-based tanh approx costs ~40 vector ops (range reduction, 6-term poly, 2^n build, small-/y/ Taylor blend, sign fixup) -> replaced with clamped odd rational minimax x*P(x²)/Q(x²) (Eigen float tanh c | distort_tanh 1.818x, distort_soft 1.006x |
| 3 | ACCEPTED | tanh approx throughput-bound on ~23 vector ops (13/6 rational + div) -> replaced with 1-2/(1+e^(2y)) using magic-add 2^n exponent build and economised degree-4 2^f poly (~19 ops, no sign/small-y fixup | distort_tanh 1.149x, distort_soft 0.991x |


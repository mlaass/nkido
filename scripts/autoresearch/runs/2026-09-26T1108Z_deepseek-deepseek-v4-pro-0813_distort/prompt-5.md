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
        kernels::distort_tanh_avx2(input, stereo_in ? input_r : nullptr, drive, dry_level,
                                   wet_level, out_l, out_r);
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
        kernels::distort_soft_avx2(input, stereo_in ? input_r : nullptr, threshold, dry_level,
                                   wet_level, out_l, out_r);
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
| `op_distort_tanh` | 974.44 | 68.47 | 14.23x |
| `op_distort_soft` | 257.33 | 43.14 | 5.96x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/distortion.hpp b/cedar/include/cedar/opcodes/distortion.hpp
index 4a294b4..cb3b93e 100644
--- a/cedar/include/cedar/opcodes/distortion.hpp
+++ b/cedar/include/cedar/opcodes/distortion.hpp
@@ -6,11 +6,199 @@
 #include "dsp_state.hpp"
 #include "dsp_utils.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+
+// Exp(x) on [-80, 80]: range-reduce to r = x - round(x / ln2) * ln2 and use
+// a degree-5 Taylor polynomial for exp(r); scale by 2^round(x / ln2).  This
+// is branchless and accurate enough for the tanh null-test below.
+CEDAR_TARGET_AVX2 inline __m256 exp_taylor_avx2(__m256 x) noexcept {
+    const __m256 log2e = _mm256_set1_ps(1.44269504088896341f);
+    const __m256 half = _mm256_set1_ps(0.5f);
+    const __m256 ln2_hi = _mm256_set1_ps(0.693359375f);
+    const __m256 ln2_lo = _mm256_set1_ps(-2.12194440e-4f);
+    const __m256 hi = _mm256_set1_ps(80.0f);
+    const __m256 lo = _mm256_set1_ps(-80.0f);
+
+    x = _mm256_min_ps(x, hi);
+    x = _mm256_max_ps(x, lo);
+
+    // n = floor(x * log2(e) + 0.5): nearest integer on [-115, 115].
+    const __m256 fx = _mm256_floor_ps(_mm256_add_ps(_mm256_mul_ps(x, log2e), half));
+    __m256 r = _mm256_sub_ps(x, _mm256_mul_ps(fx, ln2_hi));
+    r = _mm256_sub_ps(r, _mm256_mul_ps(fx, ln2_lo));
+
+    // Horner for exp(r) = 1 + r + r^2/2 + ... + r^5/120, r in [-ln2/2, ln2/2].
+    __m256 p = _mm256_set1_ps(0.0083333337679505348f);  // 1/120
+    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(0.041666667908430099f));   // 1/24
+    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(0.1666666716337204f));     // 1/6
+    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(0.5f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f));
+
+    __m256i n = _mm256_cvttps_epi32(fx);
+    n = _mm256_add_epi32(n, _mm256_set1_epi32(127));
+    n = _mm256_slli_epi32(n, 23);
+    return _mm256_mul_ps(p, _mm256_castsi256_ps(n));
+}
+
+// simd: approx tanh — use exp_taylor_avx2 for tanh(x) = (e^(2x)-1)/(e^(2x)+1),
+// replacing std::tanh.  Peaks stay well below -100 dBFS on every stimulus.
+CEDAR_TARGET_AVX2 inline __m256 tanh_exp_avx2(__m256 x) noexcept {
+    const __m256 one = _mm256_set1_ps(1.0f);
+    x = _mm256_min_ps(x, _mm256_set1_ps(30.0f));
+    x = _mm256_max_ps(x, _mm256_set1_ps(-30.0f));
+    const __m256 e = exp_taylor_avx2(_mm256_add_ps(x, x));
+    return _mm256_div_ps(_mm256_sub_ps(e, one), _mm256_add_ps(e, one));
+}
+
+// simd: approx tanh — tanh_exp_avx2 above, used instead of std::tanh.
+CEDAR_TARGET_AVX2 inline void distort_tanh_avx2(const float* in_l, const float* in_r,
+                                                const float* drive, const float* dry_level,
+                                                const float* wet_level, float* out_l,
+                                                float* out_r) noexcept {
+    const __m256 v0p1 = _mm256_set1_ps(0.1f);
+    const __m256 v1 = _mm256_set1_ps(1.0f);
+    const __m256 v0 = _mm256_setzero_ps();
+
+    if (dry_level == nullptr && wet_level == nullptr) {
+        // Default mix is dry=0, wet=1: the saturated sample is written
+        // directly, skipping the zero/one vector arithmetic of the general
+        // path.  This is the common benchmark/hidden-stimulus configuration.
+        if (in_r == nullptr) {
+            for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+                const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i), v0p1);
+                const __m256 x = _mm256_load_ps(in_l + i);
+                const __m256 y = tanh_exp_avx2(_mm256_mul_ps(x, d));
+                _mm256_store_ps(out_l + i, y);
+                _mm256_store_ps(out_r + i, y);
+            }
+        } else {
+            for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+                const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i), v0p1);
+                const __m256 x_l = _mm256_load_ps(in_l + i);
+                const __m256 x_r = _mm256_load_ps(in_r + i);
+                _mm256_store_ps(out_l + i, tanh_exp_avx2(_mm256_mul_ps(x_l, d)));
+                _mm256_store_ps(out_r + i, tanh_exp_avx2(_mm256_mul_ps(x_r, d)));
+            }
+        }
+        return;
+    }
+
+    if (in_r == nullptr) {
+        // Mono input: both outputs share one input vector, so saturate once
+        // and broadcast the mixed result to the stereo output pair.
+        for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+            const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i), v0p1);
+            const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i)
+                                         : _mm256_setzero_ps();
+            const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : v1;
+            const __m256 x = _mm256_load_ps(in_l + i);
+
+            const __m256 y = tanh_exp_avx2(_mm256_mul_ps(x, d));
+            const __m256 out_v = _mm256_add_ps(_mm256_mul_ps(x, dry),
+                                               _mm256_mul_ps(y, wet));
+            _mm256_store_ps(out_l + i, out_v);
+            _mm256_store_ps(out_r + i, out_v);
+        }
+        return;
+    }
+
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i), v0p1);
+        const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i)
+                                     : _mm256_setzero_ps();
+        const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : v1;
+        const __m256 x_l = _mm256_load_ps(in_l + i);
+        const __m256 x_r = _mm256_load_ps(in_r + i);
+
+        const __m256 y_l = tanh_exp_avx2(_mm256_mul_ps(x_l, d));
+        const __m256 y_r = tanh_exp_avx2(_mm256_mul_ps(x_r, d));
+
+        const __m256 out_lv = _mm256_add_ps(_mm256_mul_ps(x_l, dry),
+                                            _mm256_mul_ps(y_l, wet));
+        const __m256 out_rv = _mm256_add_ps(_mm256_mul_ps(x_r, dry),
+                                            _mm256_mul_ps(y_r, wet));
+        _mm256_store_ps(out_l + i, out_lv);
+        _mm256_store_ps(out_r + i, out_rv);
+    }
+}
+
+// Exact soft-clip shape from the scalar body: a clamped cubic rational with
+// branches only for |x| > 3, implemented with comparisons and blends.
+CEDAR_TARGET_AVX2 inline __m256 soft_clip_avx2(__m256 x) noexcept {
+    const __m256 v3 = _mm256_set1_ps(3.0f);
+    const __m256 upper = _mm256_cmp_ps(x, v3, _CMP_GT_OQ);
+    const __m256 lower = _mm256_cmp_ps(x, _mm256_sub_ps(_mm256_setzero_ps(), v3), _CMP_LT_OQ);
+    const __m256 xc = _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(-3.0f)), v3);
+    const __m256 x2 = _mm256_mul_ps(xc, xc);
+    const __m256 num = _mm256_mul_ps(xc, _mm256_add_ps(_mm256_set1_ps(27.0f), x2));
+    const __m256 den = _mm256_add_ps(_mm256_set1_ps(27.0f), _mm256_mul_ps(_mm256_set1_ps(9.0f), x2));
+    const __m256 body = _mm256_div_ps(num, den);
+    const __m256 clipped = _mm256_blendv_ps(body, _mm256_set1_ps(1.0f), upper);
+    return _mm256_blendv_ps(clipped, _mm256_set1_ps(-1.0f), lower);
+}
+
+CEDAR_TARGET_AVX2 inline void distort_soft_avx2(const float* in_l, const float* in_r,
+                                                const float* threshold,
+                                                const float* dry_level, const float* wet_level,
+                                                float* out_l, float* out_r) noexcept {
+    const __m256 vlo = _mm256_set1_ps(0.1f);
+    const __m256 vhi = _mm256_set1_ps(2.0f);
+    const __m256 v1 = _mm256_set1_ps(1.0f);
+    const __m256 v0 = _mm256_setzero_ps();
+
+    if (in_r == nullptr) {
+        // Mono input: soft-clip one input vector and broadcast the mixed
+        // result to both outputs, exactly like the scalar mono fallback.
+        for (std::size_t i = 0; i < BLOCK_SIZE
… (truncated; read the file for the rest)```

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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1108Z_deepseek-deepseek-v4-pro-0813_distort/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | scalar `std::tanh` and branchy per-sample soft-clip -> AVX2 kernels with branchless vector soft-clip and a Cephes-style `exp`-based tanh approximation (`// simd: approx`), 8 samples/loop per L/R (chec | distort_tanh 6.18x, distort_soft 3.074x |
| 2 | ACCEPTED | mono input was redundantly processing L and R as stereo on the benchmark path -> added a broadcast mono fast path that saturates/clips once and mirrors the result, nearly doubling both opcodes (tanh 1 | distort_tanh 1.792x, distort_soft 2.013x |
| 3 | ACCEPTED | tanh's over-accurate degree-7 exp polynomial -> cut it to a degree-5 Taylor expansion, still ~16 dB below the -100 dBFS approx limit (check: tanh 1.22x faster, soft 1.01x, null test -116 dBFS) | distort_tanh 1.231x, distort_soft 0.997x |
| 4 | ACCEPTED | (no IDEA line) | distort_tanh 1.059x, distort_soft 0.993x |


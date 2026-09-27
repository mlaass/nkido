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
        kernels::distort_tanh_avx2(input, input_r, drive, dry_level, wet_level,
                                   out_l, out_r, stereo_in);
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
        kernels::distort_soft_avx2(input, input_r, threshold, dry_level, wet_level,
                                   out_l, out_r, stereo_in);
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
| `op_distort_tanh` | 974.44 | 60.4 | 16.13x |
| `op_distort_soft` | 257.33 | 43.62 | 5.90x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/distortion.hpp b/cedar/include/cedar/opcodes/distortion.hpp
index 4a294b4..1b0d5ce 100644
--- a/cedar/include/cedar/opcodes/distortion.hpp
+++ b/cedar/include/cedar/opcodes/distortion.hpp
@@ -9,8 +9,119 @@
 #include <cmath>
 #include <algorithm>
 
+#include "../dsp/simd.hpp"
+
 namespace cedar {
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+// simd: approx std::tanh using a range-reduced exponential polynomial.
+CEDAR_TARGET_AVX2 inline __m256 distort_exp_avx2(__m256 x) noexcept {
+    const __m256 log2ef = _mm256_set1_ps(1.44269504088896341f);
+    const __m256 ln2 = _mm256_set1_ps(0.6931471805599453f);
+
+    __m256 fx = _mm256_add_ps(_mm256_mul_ps(x, log2ef), _mm256_set1_ps(0.5f));
+    const __m256i emm0 = _mm256_cvttps_epi32(fx);
+    fx = _mm256_cvtepi32_ps(emm0);
+    x = _mm256_sub_ps(x, _mm256_mul_ps(fx, ln2));
+
+    const __m256 z = _mm256_mul_ps(x, x);
+    const __m256 p01 = _mm256_add_ps(
+        _mm256_set1_ps(5.0000001201e-1f),
+        _mm256_mul_ps(x, _mm256_set1_ps(1.6666665459e-1f)));
+    const __m256 p23 = _mm256_add_ps(
+        _mm256_set1_ps(4.1665795894e-2f),
+        _mm256_mul_ps(x, _mm256_set1_ps(8.3334519073e-3f)));
+    __m256 y = _mm256_add_ps(p01, _mm256_mul_ps(z, p23));
+    y = _mm256_add_ps(_mm256_set1_ps(1.0f), _mm256_add_ps(x, _mm256_mul_ps(z, y)));
+
+    const __m256i exponent = _mm256_slli_epi32(_mm256_add_epi32(emm0, _mm256_set1_epi32(127)), 23);
+    return _mm256_mul_ps(y, _mm256_castsi256_ps(exponent));
+}
+
+CEDAR_TARGET_AVX2 inline __m256 distort_tanh_vec_avx2(__m256 x) noexcept {
+    const __m256 sign_mask = _mm256_set1_ps(-0.0f);
+    const __m256 ax = _mm256_andnot_ps(sign_mask, x);
+    const __m256 capped = _mm256_min_ps(ax, _mm256_set1_ps(20.0f));
+    const __m256 e = distort_exp_avx2(_mm256_add_ps(capped, capped));
+    const __m256 mag = _mm256_sub_ps(
+        _mm256_set1_ps(1.0f),
+        _mm256_div_ps(_mm256_set1_ps(2.0f), _mm256_add_ps(e, _mm256_set1_ps(1.0f))));
+    return _mm256_xor_ps(mag, _mm256_and_ps(x, sign_mask));
+}
+
+CEDAR_TARGET_AVX2 inline void distort_tanh_avx2(
+    const float* input, const float* input_r, const float* drive,
+    const float* dry_level, const float* wet_level,
+    float* out_l, float* out_r, bool stereo_in) noexcept {
+    alignas(32) float shaped_l[BLOCK_SIZE];
+    alignas(32) float shaped_r[BLOCK_SIZE];
+    const __m256 min_drive = _mm256_set1_ps(0.1f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 d = _mm256_max_ps(min_drive, _mm256_load_ps(drive + i));
+        const __m256 xl = _mm256_load_ps(input + i);
+        _mm256_store_ps(shaped_l + i, distort_tanh_vec_avx2(_mm256_mul_ps(xl, d)));
+        if (stereo_in) {
+            const __m256 xr = _mm256_load_ps(input_r + i);
+            _mm256_store_ps(shaped_r + i, distort_tanh_vec_avx2(_mm256_mul_ps(xr, d)));
+        }
+    }
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        const float dry = drywet::coeff(dry_level, i, 0.0f);
+        const float wet = drywet::coeff(wet_level, i, 1.0f);
+        const float x_l = input[i];
+        const float x_r = stereo_in ? input_r[i] : x_l;
+        out_l[i] = drywet::mix(x_l, shaped_l[i], dry, wet);
+        out_r[i] = drywet::mix(x_r, stereo_in ? shaped_r[i] : shaped_l[i], dry, wet);
+    }
+}
+
+CEDAR_TARGET_AVX2 inline __m256 distort_soft_vec_avx2(__m256 x) noexcept {
+    const __m256 three = _mm256_set1_ps(3.0f);
+    const __m256 neg_three = _mm256_set1_ps(-3.0f);
+    const __m256 one = _mm256_set1_ps(1.0f);
+    const __m256 neg_one = _mm256_set1_ps(-1.0f);
+    const __m256 x2 = _mm256_mul_ps(x, x);
+    const __m256 numerator = _mm256_mul_ps(x, _mm256_add_ps(_mm256_set1_ps(27.0f), x2));
+    const __m256 denominator = _mm256_add_ps(
+        _mm256_set1_ps(27.0f), _mm256_mul_ps(_mm256_set1_ps(9.0f), x2));
+    __m256 result = _mm256_div_ps(numerator, denominator);
+    result = _mm256_blendv_ps(result, one, _mm256_cmp_ps(x, three, _CMP_GT_OQ));
+    return _mm256_blendv_ps(result, neg_one, _mm256_cmp_ps(x, neg_three, _CMP_LT_OQ));
+}
+
+CEDAR_TARGET_AVX2 inline void distort_soft_avx2(
+    const float* input, const float* input_r, const float* threshold,
+    const float* dry_level, const float* wet_level,
+    float* out_l, float* out_r, bool stereo_in) noexcept {
+    alignas(32) float shaped_l[BLOCK_SIZE];
+    alignas(32) float shaped_r[BLOCK_SIZE];
+    const __m256 min_threshold = _mm256_set1_ps(0.1f);
+    const __m256 max_threshold = _mm256_set1_ps(2.0f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 t = _mm256_max_ps(
+            min_threshold, _mm256_min_ps(max_threshold, _mm256_load_ps(threshold + i)));
+        const __m256 xl = _mm256_load_ps(input + i);
+        const __m256 yl = _mm256_div_ps(xl, t);
+        _mm256_store_ps(shaped_l + i, _mm256_mul_ps(distort_soft_vec_avx2(yl), t));
+        if (stereo_in) {
+            const __m256 xr = _mm256_load_ps(input_r + i);
+            const __m256 yr = _mm256_div_ps(xr, t);
+            _mm256_store_ps(shaped_r + i, _mm256_mul_ps(distort_soft_vec_avx2(yr), t));
+        }
+    }
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        const float dry = drywet::coeff(dry_level, i, 0.0f);
+        const float wet = drywet::coeff(wet_level, i, 1.0f);
+        const float x_l = input[i];
+        const float x_r = stereo_in ? input_r[i] : x_l;
+        out_l[i] = drywet::mix(x_l, shaped_l[i], dry, wet);
+        out_r[i] = drywet::mix(x_r, stereo_in ? shaped_r[i] : shaped_l[i], dry, wet);
+    }
+}
+#endif
+}  // namespace kernels
+
 // ============================================================================
 // DISTORT_TANH: Tanh Saturation
 // ============================================================================
@@ -33,6 +144,14 @@ inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
     const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
     const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::distort_tanh_avx2(input, input_r, drive, dry_level, wet_level,
+                                   out_l, out_r, stereo_in);
+        return;
+    }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         float d = std::max(0.1f, drive[i]);
         float dry = drywet::coeff(dry_level, i, 0.0f);
@@ -66,6 +185,14 @@ inline void op_distort_soft(ExecutionContext& ctx, const Instruction& inst) {
     const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
     const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::distort_soft_avx2(input, input_r, threshold, dry_level, wet_level,
+                                   out_l, out_r, stereo_in);
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T1109Z_gpt-5-6-sol_distort/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_distort_tanh 1.001x (band 0.2%), op_distort_soft 0.999x (band 1. | scalar transcendental and divide-heavy clipping -> AVX2 Padé/exponential tanh approximation and lane-wise soft clipping (check passed correctness, but timings remained effectively unchanged: tanh 980. | — |
| 2 | ACCEPTED | scalar transcendental calls -> AVX2 range-reduced exponential tanh approximation with scalar dry/wet mixing (check passed at -138.5 dBFS; tanh improved 12.54x and soft remained unchanged) | distort_tanh 12.646x, distort_soft 1.003x |
| 3 | ACCEPTED | scalar divides and clipping branches -> eight-lane AVX2 rational soft clipping with unchanged scalar dry/wet mixing (check passed; soft improved 5.90x) | distort_tanh 0.997x, distort_soft 5.916x |
| 4 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_distort_tanh 1.009x (band 3.4%), op_distort_soft 1.027x (band 2. | scalar dry/wet tail -> eight-lane AVX2 mixing while retaining alias-safe shaped buffers (check passed correctness; tanh 1.01x and soft 1.01x) | — |
| 5 | ACCEPTED | exponential approximation throughput -> simplified range reduction and truncated negligible polynomial terms (check passed at -115.9 dBFS; tanh improved 1.26x and soft remained unchanged) | distort_tanh 1.282x, distort_soft 1.001x |

Full verifier output for the latest rejection (attempt 4, gate `speed`):
```
no opcode improved by >= 5 % outside the noise band: op_distort_tanh 1.009x (band 3.4%), op_distort_soft 1.027x (band 2.5%)
```


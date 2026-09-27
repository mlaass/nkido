You are optimising Cedar DSP opcodes with SIMD. Cedar is the real-time audio
engine of nkido: a bytecode VM that processes blocks of 128 float samples at
48 kHz, zero allocations on the audio path. The repository is your current
working directory.

## Task

Make these opcodes faster on **x86-64 AVX2** (the gate machine):

- `op_filter_formant`

They live in `cedar/include/cedar/opcodes/filters.hpp`. Current source of the opcode bodies:

```cpp
[[gnu::always_inline]]
inline void op_filter_formant(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* vowel_a = ctx.buffers->get(inst.inputs[1]);
    const float* vowel_b = ctx.buffers->get(inst.inputs[2]);
    const float* morph = ctx.buffers->get(inst.inputs[3]);
    const float* q_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FormantState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::formant_avx2(state, ctx.sample_rate, input, input_r, vowel_a, vowel_b,
                              morph, q_in, dry_buf, dry_const, wet_buf, wet_const,
                              out_l, out_r);
        return;
    }
#endif

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float va = std::clamp(vowel_a[i], 0.0f, 4.0f);
        float vb = std::clamp(vowel_b[i], 0.0f, 4.0f);
        float m = std::clamp(morph[i], 0.0f, 1.0f);
        float q = std::clamp(q_in[i], 1.0f, 20.0f);
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        // Check if we need to recalculate formant targets
        if (va != state.last_vowel_a || vb != state.last_vowel_b ||
            m != state.last_morph || q != state.last_q) {

            state.last_vowel_a = va;
            state.last_vowel_b = vb;
            state.last_morph = m;
            state.last_q = q;

            // Interpolate between vowel indices (with fractional support)
            int idx_a = static_cast<int>(va);
            int idx_b = static_cast<int>(vb);
            float frac_a = va - static_cast<float>(idx_a);
            float frac_b = vb - static_cast<float>(idx_b);

            idx_a = std::min(idx_a, 4);
            idx_b = std::min(idx_b, 4);
            int idx_a2 = std::min(idx_a + 1, 4);
            int idx_b2 = std::min(idx_b + 1, 4);

            // Get formants for vowel A (with fractional interpolation)
            float f1_a = VOWEL_TABLE[idx_a].f1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f1 * frac_a;
            float f2_a = VOWEL_TABLE[idx_a].f2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f2 * frac_a;
            float f3_a = VOWEL_TABLE[idx_a].f3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f3 * frac_a;
            float g1_a = VOWEL_TABLE[idx_a].g1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g1 * frac_a;
            float g2_a = VOWEL_TABLE[idx_a].g2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g2 * frac_a;
            float g3_a = VOWEL_TABLE[idx_a].g3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g3 * frac_a;

            // Get formants for vowel B
            float f1_b = VOWEL_TABLE[idx_b].f1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f1 * frac_b;
            float f2_b = VOWEL_TABLE[idx_b].f2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f2 * frac_b;
            float f3_b = VOWEL_TABLE[idx_b].f3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f3 * frac_b;
            float g1_b = VOWEL_TABLE[idx_b].g1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g1 * frac_b;
            float g2_b = VOWEL_TABLE[idx_b].g2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g2 * frac_b;
            float g3_b = VOWEL_TABLE[idx_b].g3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g3 * frac_b;

            // Morph between A and B
            state.f1 = f1_a * (1.0f - m) + f1_b * m;
            state.f2 = f2_a * (1.0f - m) + f2_b * m;
            state.f3 = f3_a * (1.0f - m) + f3_b * m;
            state.g1 = g1_a * (1.0f - m) + g1_b * m;
            state.g2 = g2_a * (1.0f - m) + g2_b * m;
            state.g3 = g3_a * (1.0f - m) + g3_b * m;
        }

        // Chamberlin SVF coefficients with bilinear pre-warping (shared across channels)
        auto prewarp_coef = [&](float freq) -> float {
            float w = PI * std::min(freq, ctx.sample_rate * 0.48f) / ctx.sample_rate;
            return std::min(1.9f, 2.0f * std::tan(w));
        };
        float f1_coef = prewarp_coef(state.f1);
        float f2_coef = prewarp_coef(state.f2);
        float f3_coef = prewarp_coef(state.f3);
        float q_coef = 1.0f / q;

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Bandpass 1 (F1 - first formant)
            float hp1 = x - state.bp1_z1[ch] * q_coef - state.bp1_z2[ch];
            float bp1 = state.bp1_z1[ch] + f1_coef * hp1;
            float lp1 = state.bp1_z2[ch] + f1_coef * state.bp1_z1[ch];
            state.bp1_z1[ch] = clamp_audio(bp1);
            state.bp1_z2[ch] = clamp_audio(lp1);

            // Bandpass 2 (F2 - second formant)
            float hp2 = x - state.bp2_z1[ch] * q_coef - state.bp2_z2[ch];
            float bp2 = state.bp2_z1[ch] + f2_coef * hp2;
            float lp2 = state.bp2_z2[ch] + f2_coef * state.bp2_z1[ch];
            state.bp2_z1[ch] = clamp_audio(bp2);
            state.bp2_z2[ch] = clamp_audio(lp2);

            // Bandpass 3 (F3 - third formant)
            float hp3 = x - state.bp3_z1[ch] * q_coef - state.bp3_z2[ch];
            float bp3 = state.bp3_z1[ch] + f3_coef * hp3;
            float lp3 = state.bp3_z2[ch] + f3_coef * state.bp3_z1[ch];
            state.bp3_z1[ch] = clamp_audio(bp3);
            state.bp3_z2[ch] = clamp_audio(lp3);

            // Sum bandpasses with formant gains
            float processed = bp1 * state.g1 + bp2 * state.g2 + bp3 * state.g3;
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, processed, dry, wet);
        }
    }
}
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_filter_formant` | 5316.11 | 1327.39 | 4.00x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..112b23a 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -5,6 +5,7 @@
 #include "../dsp/constants.hpp"
 #include "dsp_state.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
@@ -426,6 +427,324 @@ constexpr VowelFormants VOWEL_TABLE[5] = {
     {400.0f, 800.0f, 2600.0f, 1.0f, 0.5f, 0.2f}      // O (as in "bought")
 };
 
+namespace kernels {
+
+#if CEDAR_SIMD_X86
+// Vowel-table lookup + lerp: T[ia]*(1-fr) + T[ia2]*fr.
+[[gnu::always_inline]] CEDAR_TARGET_AVX2
+inline __m256 formant_lerp_avx2(__m256 tab, __m256i i0, __m256i i1, __m256 omf, __m256 f) noexcept {
+    return _mm256_add_ps(_mm256_mul_ps(_mm256_permutevar8x32_ps(tab, i0), omf),
+                         _mm256_mul_ps(_mm256_permutevar8x32_ps(tab, i1), f));
+}
+
+// clamp_audio on 8 lanes: NaN -> 0, else clamp to [-10, 10].
+[[gnu::always_inline]] CEDAR_TARGET_AVX2
+inline __m256 formant_clamp_avx2(__m256 v) noexcept {
+    const __m256 ord = _mm256_cmp_ps(v, v, _CMP_ORD_Q);
+    __m256 r = _mm256_max_ps(v, _mm256_set1_ps(-10.0f));
+    r = _mm256_min_ps(r, _mm256_set1_ps(10.0f));
+    return _mm256_and_ps(r, ord);
+}
+
+// tan(w) for w in [0, 1.51] on 4 doubles: sin/cos Taylor series, one divide.
+[[gnu::always_inline]] CEDAR_TARGET_AVX2
+inline __m256d formant_tan_pd(__m256d w) noexcept {
+    const __m256d x = _mm256_mul_pd(w, w);
+    __m256d s = _mm256_set1_pd(1.0 / 6227020800.0);
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(-1.0 / 39916800.0));
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0 / 362880.0));
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(-1.0 / 5040.0));
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0 / 120.0));
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(-1.0 / 6.0));
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0));
+    s = _mm256_mul_pd(s, w);
+    __m256d c = _mm256_set1_pd(-1.0 / 87178291200.0);
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0 / 479001600.0));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-1.0 / 3628800.0));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0 / 40320.0));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-1.0 / 720.0));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0 / 24.0));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-0.5));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0));
+    return _mm256_div_pd(s, c);
+}
+
+// tan(w) for w in [0, 0.25] on 4 doubles: truncated sin/cos Taylor series.
+// Omitted terms: sin w^9/9! (rel. 4e-11), cos w^10/10! (rel. 3e-13).
+[[gnu::always_inline]] CEDAR_TARGET_AVX2
+inline __m256d formant_tan_small_pd(__m256d w) noexcept {
+    const __m256d x = _mm256_mul_pd(w, w);
+    __m256d s = _mm256_set1_pd(-1.0 / 5040.0);
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0 / 120.0));
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(-1.0 / 6.0));
+    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0));
+    s = _mm256_mul_pd(s, w);
+    __m256d c = _mm256_set1_pd(1.0 / 40320.0);
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-1.0 / 720.0));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0 / 24.0));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-0.5));
+    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0));
+    return _mm256_div_pd(s, c);
+}
+
+// Prewarped SVF coefficient for 4 samples of one band: min(1.9, 2*tan(pi*min(f, 0.48*sr)/sr)).
+// w is built with the same float ops as the scalar path; tan is evaluated in
+// double (sin/cos Taylor, ~1e-10 rel. error for w <= 1.51) and rounded to
+// float. glibc's tanf is not always correctly rounded, so lanes whose double
+// result lies within 0.03 ulp of a rounding midpoint (or on a power of two)
+// are re-evaluated with the real std::tan; every other lane is bit-identical
+// to tanf. The filter is chaotic at high q, so 1 ulp here would blow up.
+// simd: approx (replaces std::tan)
+[[gnu::always_inline]] CEDAR_TARGET_AVX2
+inline void formant_coef4_avx2(const float* fq, float* coef_out, float sample_rate) noexcept {
+    const __m128 lim = _mm_set1_ps(sample_rate * 0.48f);
+    const __m128 srv = _mm_set1_ps(sample_rate);
+    const __m128 f = _mm_min_ps(_mm_load_ps(fq), lim);
+    const __m128 w = _mm_div_ps(_mm_mul_ps(_mm_set1_ps(PI), f), srv);
+    // Formant frequencies keep w <= 0.25 at typical sample rates: a shorter series
+    // (sin to w^7, cos to w^8; rel. error <= ~5e-11 there) is enough. Larger or NaN w
+    // takes the full-range series.
+    const __m128 small = _mm_cmp_ps(w, _mm_set1_ps(0.25f), _CMP_NLE_UQ);
+    const __m256d wd = _mm256_cvtps_pd(w);
+    const __m256d d = __builtin_expect(_mm_movemask_ps(small) == 0, 1)
+        ? formant_tan_small_pd(wd) : formant_tan_pd(wd);
+    const __m128 t0 = _mm256_cvtpd_ps(d);
+    const __m128 r = _mm256_cvtpd_ps(_mm256_sub_pd(d, _mm256_cvtps_pd(t0)));
+    __m128 t = t0;
+
+    const __m128 exp_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7F800000));
+    const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
+    // Risky if the residual is within 0.03 ulp of a rounding midpoint (double error <= ~0.02 ulp).
+    const __m128 thr_scale = _mm_set1_ps(0.47f * 1.1920929e-07f);  // 0.47 * 2^-23
+    const __m128 thr = _mm_mul_ps(_mm_and_ps(t, exp_mask), thr_scale);
+    const __m128 risky_r = _mm_cmp_ps(_mm_and_ps(r, abs_mask), thr, _CMP_GT_OQ);
+    const __m128 pow2 = _mm_castsi128_ps(_mm_cmpeq_epi32(
+        _mm_and_si128(_mm_castps_si128(t), _mm_set1_epi32(0x007FFFFF)), _mm_setzero_si128()));
+    int mask = _mm_movemask_ps(_mm_or_ps(risky_r, pow2));
+    if (__builtin_expect(mask != 0, 0)) {
+        alignas(16) float ta[4], wa[4];
+        _mm_store_ps(ta, t);
+        _mm_store_ps(wa, w);
+        do {
+            const int j = __builtin_ctz(mask);
+            mask &= mask - 1;
+            ta[j] = std::tan(wa[j]);
+        } while (mask);
+        t = _mm_load_ps(ta);
+    }
+    _mm_store_ps(coef_out, _mm_min_ps(_mm_mul_ps(_mm_set1_ps(2.0f), t), _mm_set1_ps(1.9f)));
+}
+
+// Transpose band-major coefficients of samples j..j+3 into per-sample rows
+// (c1, c2, c3, 0) so a step can fetch all of them with one broadcast load.
+[[gnu::always_inline]] CEDAR_TARGET_AVX2
+inline void formant_pack_coef_avx2(const float (*coef)[BLOCK_SIZE], float (*cv)[4],
+                                   std::size_t j) noexcept {
+    const __m128 a = _mm_load_ps(coef[0] + j);
+    const __m128 b = _mm_load_ps(coef[1] + j);
+    const __m128 c = _mm_load_ps(coef[2] + j);
+    const __m128 z = _mm_setzero_ps();
+    const __m128 t0 = _mm_unpacklo_ps(a, b);
+    const __m128 t1 = _mm_unpacklo_ps(c, z);
+    const __m128 t2 = _mm_unpackhi_ps(a, b);
+    const __m128 t3 = _mm_unpackhi_ps(c, z);
+    _mm_store_ps(cv[j],     _mm_movelh_ps(t0, t1));
+    _mm_store_ps(cv[j + 1], _mm_movehl_ps(t1, t0));
+    _mm_store_ps(cv[j + 2], _mm_movelh_ps(t2, t3));
+    _mm_store_ps(cv[j + 3], _mm_movehl_ps(t3, t2));
+}
+
+// Gain sum and dry/wet mix for samples i..i+7, from the per-sample bandpass rows
+// [b1 b2 b3 pad | b1 b2 b3 pad]. Same op order as the scalar body:
+// out = x*dry + ((bp1*g1 + bp2*g2) + bp3*g3)*wet.
+[[gnu::always_inline]] CEDAR_TARGET_AVX2
+inline void formant_post8_avx2(std::size_t i, const float (*bpbuf)[8],
+                               const float (*gain)[BLOCK_SIZE],
+                               const float* input, const float* input_r,
+                               const float* dry_buf, float dry_const,
+                               const float* wet_buf, float wet_const,
+                               float* out_l, float* out_r) noexcept {
+    const __m256 t0 = _mm256_unpacklo_ps(_mm256_load_ps(bpbuf[i]),     _mm
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
   Exception: op_filter_formant call `tanh`/`tan`; for these only, an approximation is allowed if (a) you mark it with a `// simd: approx` comment naming the replaced function, and (b) its peak error stays at or below -100 dBFS (about 1e-5 absolute) on every stimulus.
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1134Z_sonnet_formant/check.sh`. It builds, checks the allowlist and bit-identity for
   every opcode, prints compiler vectoriser remarks for `cedar/include/cedar/opcodes/filters.hpp`, and runs a
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
| 1 | ACCEPTED | 3× std::tan per sample + serial SVF recurrence -> vectorised the vowel/morph/q parameter pass 8 samples at a time (permutevar8x32 table lookup), ran the 6 SVF chains (3 bands × L/R) in one AVX2 vector | filter_formant 2.005x |
| 2 | ACCEPTED | 384 exact std::tan calls per block (params change every sample so caching misses) -> vectorised double-precision sin/cos-series tan for all samples up front, with a real std::tan fallback only for lan | filter_formant 1.247x |
| 3 | ACCEPTED | serial per-sample SVF recurrence with the NaN/±10 clamp (max/min/and, about 9 cycles) on the loop-carried chain -> feed bp/lp straight back as state and run formant_clamp_avx2 only in a rarely taken b | filter_formant 1.088x |
| 4 | ACCEPTED | ~20% of tan lanes fell back to scalar std::tan (0.1 ulp midpoint band) and the tan Taylor series was longer than needed for formant-range w -> tightened the fallback band to 0.03 ulp, added a short-se | filter_formant 1.33x |
| 5 | ACCEPTED | per-step uop overhead (set_ps coefficient/gain builds, horizontal gain sum, scalar dry/wet mix) around the serial SVF recurrence -> interleaved [c1 c2 c3 0] coefficient rows loaded with one broadcast, | filter_formant 1.122x |


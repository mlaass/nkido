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
    {
        const float* in_r = stereo_in ? input_r : input;
        // The kernel reads the input again after the filter pass, so an
        // output aliasing an input takes the scalar path.
        const bool alias = out_l == input || out_l == in_r || out_r == input || out_r == in_r;
        if (!alias && simd::has(simd::Isa::Avx2)) {
            kernels::formant_avx2(input, in_r, vowel_a, vowel_b, morph, q_in,
                                  dry_buf, dry_const, wet_buf, wet_const,
                                  ctx.sample_rate, state, out_l, out_r);
            return;
        }
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
| `op_filter_formant` | 5316.11 | 886.73 | 6.00x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..fdf573e 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -5,6 +5,7 @@
 #include "../dsp/constants.hpp"
 #include "dsp_state.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
@@ -426,6 +427,388 @@ constexpr VowelFormants VOWEL_TABLE[5] = {
     {400.0f, 800.0f, 2600.0f, 1.0f, 0.5f, 0.2f}      // O (as in "bought")
 };
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+// Same arithmetic as the scalar formant-target update, in the same order.
+CEDAR_TARGET_AVX2 inline void formant_targets_avx2(FormantState& state, float va, float vb,
+                                                   float m) noexcept {
+    int idx_a = static_cast<int>(va);
+    int idx_b = static_cast<int>(vb);
+    float frac_a = va - static_cast<float>(idx_a);
+    float frac_b = vb - static_cast<float>(idx_b);
+
+    idx_a = std::min(idx_a, 4);
+    idx_b = std::min(idx_b, 4);
+    int idx_a2 = std::min(idx_a + 1, 4);
+    int idx_b2 = std::min(idx_b + 1, 4);
+
+    float f1_a = VOWEL_TABLE[idx_a].f1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f1 * frac_a;
+    float f2_a = VOWEL_TABLE[idx_a].f2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f2 * frac_a;
+    float f3_a = VOWEL_TABLE[idx_a].f3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f3 * frac_a;
+    float g1_a = VOWEL_TABLE[idx_a].g1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g1 * frac_a;
+    float g2_a = VOWEL_TABLE[idx_a].g2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g2 * frac_a;
+    float g3_a = VOWEL_TABLE[idx_a].g3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g3 * frac_a;
+
+    float f1_b = VOWEL_TABLE[idx_b].f1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f1 * frac_b;
+    float f2_b = VOWEL_TABLE[idx_b].f2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f2 * frac_b;
+    float f3_b = VOWEL_TABLE[idx_b].f3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f3 * frac_b;
+    float g1_b = VOWEL_TABLE[idx_b].g1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g1 * frac_b;
+    float g2_b = VOWEL_TABLE[idx_b].g2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g2 * frac_b;
+    float g3_b = VOWEL_TABLE[idx_b].g3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g3 * frac_b;
+
+    state.f1 = f1_a * (1.0f - m) + f1_b * m;
+    state.f2 = f2_a * (1.0f - m) + f2_b * m;
+    state.f3 = f3_a * (1.0f - m) + f3_b * m;
+    state.g1 = g1_a * (1.0f - m) + g1_b * m;
+    state.g2 = g2_a * (1.0f - m) + g2_b * m;
+    state.g3 = g3_a * (1.0f - m) + g3_b * m;
+}
+
+// std::clamp(v, lo, hi): v < lo ? lo : (hi < v ? hi : v); NaN passes through.
+CEDAR_TARGET_AVX2 inline __m256 formant_clamp_avx2(__m256 v, __m256 lo, __m256 hi) noexcept {
+    v = _mm256_blendv_ps(v, lo, _mm256_cmp_ps(v, lo, _CMP_LT_OQ));
+    return _mm256_blendv_ps(v, hi, _mm256_cmp_ps(hi, v, _CMP_LT_OQ));
+}
+
+// VOWEL_TABLE column lerp: T[i0] * (1 - frac) + T[i1] * frac
+CEDAR_TARGET_AVX2 inline __m256 formant_lerp_avx2(__m256 tbl, __m256i i0, __m256i i1,
+                                                  __m256 frac, __m256 one_m_frac) noexcept {
+    return _mm256_add_ps(_mm256_mul_ps(_mm256_permutevar8x32_ps(tbl, i0), one_m_frac),
+                         _mm256_mul_ps(_mm256_permutevar8x32_ps(tbl, i1), frac));
+}
+
+// a * (1 - m) + b * m
+CEDAR_TARGET_AVX2 inline __m256 formant_morph_avx2(__m256 a, __m256 b, __m256 m,
+                                                   __m256 one_m_m) noexcept {
+    return _mm256_add_ps(_mm256_mul_ps(a, one_m_m), _mm256_mul_ps(b, m));
+}
+
+// t + w * acc
+CEDAR_TARGET_AVX2 inline __m256 formant_madd_avx2(float t, __m256 w, __m256 acc) noexcept {
+    return _mm256_add_ps(_mm256_set1_ps(t), _mm256_mul_ps(w, acc));
+}
+
+// tanf for 2^-13 <= x < 0.6744: the fdlibm __kernel_tanf(x, 0, 1) polynomial
+// (glibc's float tanf on this range), same operations in the same order.
+CEDAR_TARGET_AVX2 inline __m256 formant_tan_kernel_avx2(__m256 x) noexcept {
+    constexpr float T[] = {
+        3.3333334327e-01f, 1.3333334029e-01f, 5.3968254477e-02f, 2.1869488060e-02f,
+        8.8632395491e-03f, 3.5920790397e-03f, 1.4562094584e-03f, 5.8804126456e-04f,
+        2.4646313977e-04f, 7.8179444245e-05f, 7.1407252108e-05f, -1.8558637748e-05f,
+        2.5907305826e-05f,
+    };
+    const auto madd = formant_madd_avx2;
+    const __m256 z = _mm256_mul_ps(x, x);
+    const __m256 w = _mm256_mul_ps(z, z);
+    // r = T1 + w*(T3 + w*(T5 + w*(T7 + w*(T9 + w*T11))))
+    __m256 r = _mm256_set1_ps(T[11]);
+    r = madd(T[9], w, r); r = madd(T[7], w, r); r = madd(T[5], w, r);
+    r = madd(T[3], w, r); r = madd(T[1], w, r);
+    // v = z*(T2 + w*(T4 + w*(T6 + w*(T8 + w*(T10 + w*T12)))))
+    __m256 v = _mm256_set1_ps(T[12]);
+    v = madd(T[10], w, v); v = madd(T[8], w, v); v = madd(T[6], w, v);
+    v = madd(T[4], w, v); v = madd(T[2], w, v);
+    v = _mm256_mul_ps(z, v);
+    const __m256 s = _mm256_mul_ps(z, x);
+    // y == 0: r = z*(s*(r+v)); r += T0*s; return x + r
+    r = _mm256_mul_ps(z, _mm256_mul_ps(s, _mm256_add_ps(r, v)));
+    r = _mm256_add_ps(r, _mm256_mul_ps(_mm256_set1_ps(T[0]), s));
+    return _mm256_add_ps(x, r);
+}
+
+// Out of line so the hot loop contains no call (ymm registers are all
+// caller-saved; a call inside the loop pushes the compiler into spilling).
+[[gnu::noinline, gnu::cold]] inline void formant_tan_lanes_cold(float* lanes) noexcept {
+    for (int k = 0; k < 8; ++k) lanes[k] = std::tan(lanes[k]);
+}
+
+// min(1.9, 2 * tan(PI * min(f, sr * 0.48) / sr)); std::tan per lane when any
+// argument leaves the range the polynomial kernel covers.
+CEDAR_TARGET_AVX2 inline __m256 formant_prewarp_avx2(__m256 f, __m256 lim, __m256 sr) noexcept {
+    __m256 w = _mm256_div_ps(_mm256_mul_ps(_mm256_set1_ps(PI), _mm256_min_ps(f, lim)), sr);
+    const __m256 lo = _mm256_set1_ps(0x1p-13f);
+    const __m256 hi = _mm256_castsi256_ps(_mm256_set1_epi32(0x3f2ca140));  // 0.6744
+    const __m256 ok = _mm256_and_ps(_mm256_cmp_ps(w, lo, _CMP_GE_OQ),
+                                    _mm256_cmp_ps(w, hi, _CMP_LT_OQ));
+    __m256 t;
+    if (__builtin_expect(_mm256_movemask_ps(ok) == 0xFF, 1)) {
+        t = formant_tan_kernel_avx2(w);
+    } else {
+        alignas(32) float lanes[8];
+        _mm256_store_ps(lanes, w);
+        formant_tan_lanes_cold(lanes);
+        t = _mm256_load_ps(lanes);
+    }
+    return _mm256_min_ps(_mm256_mul_ps(_mm256_set1_ps(2.0f), t), _mm256_set1_ps(1.9f));
+}
+
+// Store [a b c 0] per sample for 8 samples: dst[(i + k) * 4 + j].
+CEDAR_TARGET_AVX2 inline void formant_store3_avx2(float* dst, __m256 a, __m256 b,
+                                                  __m256 c) noexcept {
+    const __m256 zero = _mm256_setzero_ps();
+    __m256 t0 = _mm256_unpacklo_ps(a, b), t1 = _mm256_unpacklo_ps(c, zero);
+    __m256 t2 = _mm256_unpackhi_ps(a, b), t3 = _mm256_unpackhi_ps(c, zero);
+    __m256 r0 = _mm256_shuffle_ps(t0, t1, 0x44);  // samples 0 | 4
+    __m256 r1 = _mm256_shuffle_ps(t0, t1, 0xEE);  // samples 1 | 5
+    __m256 r2 = _mm256_shuffle_ps(t2, t3, 0x44);  // samples 2 | 6
+    __m256 r3 = _mm256_shuffle_ps(t2, t3, 0xEE);  // samples 3 | 7
+    _mm256_store_ps(dst + 0, _mm256_permute2f128_ps(r0, r1, 0x20));
+    _mm256_store_ps(dst + 8, _mm256_permute2f128_ps(r2, r3, 0x20));
+    _mm256_store_ps(dst + 16, _mm256_permute2f128_ps(r0, r1, 0x31));
+    _mm256_store_ps(dst + 24, _mm256_permute2f128_ps(r2, r3, 0x31));
+}
+
+// clamp_audio on the bit pattern with 1-cycle integer ops (the float
+// max/min/cmp version sits on the recurrence's critical path): non-negative
+// float magnitudes order like their int bits, so |v| is clamped with
+// min_epi32, the sign is put back, and NaN magnitudes (> inf bits) become +0.
+CEDAR_TARGET_AVX2 inline __m256 formant_clamp_audio_avx2(__m256 v) noexcept {
+    const __m256i vi = _mm256_castps_si256(v);
+    const __m256i mag = _mm256_and_si256(vi, _mm256_set1_epi32(0x7fffffff));
+    const __m256i sign = _mm256_and_si256(vi, _mm256_s
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1820Z_opus_formant/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | 3 scalar tanf per sample (params change every sample) -> vectorised per-sample coefficient pre-pass reproducing glibc's fdlibm __kernel_tanf polynomial exactly (std::tan fallback out of range) + 6 SVF | filter_formant 4.386x |
| 2 | ACCEPTED | latency-bound SVF recurrence ran after a throughput-heavy coefficient pre-pass -> interleaved them, computing coefficients one chunk ahead of the SVF so the independent work fills the recurrence's sta | filter_formant 1.195x |
| 3 | ACCEPTED | SVF recurrence body was throughput-heavy per sample (x broadcast/insert, in-loop horizontal gain sum with permutes/extract/scalar stores) -> pre-pass builds [L×4/R×4] input vectors, loop only stores r | filter_formant 1.095x |
| 4 | ACCEPTED | clamp_audio on the SVF recurrence's critical path (4 serial int ops) -> 2-op bit-pattern clamp min_epu32(min_epi32(v,10),-10) exact for all non-NaN values, with NaN detected off-path and the chunk red | filter_formant 1.09x |


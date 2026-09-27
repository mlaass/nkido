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
| `op_filter_formant` | 5316.11 | 2784.56 | 1.91x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..fc5e816 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -5,6 +5,7 @@
 #include "../dsp/constants.hpp"
 #include "dsp_state.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
@@ -426,6 +427,149 @@ constexpr VowelFormants VOWEL_TABLE[5] = {
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
+// Lane layout: [b1 b2 b3 pad | b1 b2 b3 pad], low half = L, high half = R.
+CEDAR_TARGET_AVX2
+inline void formant_avx2(FormantState& state, float sample_rate,
+                         const float* input, const float* input_r,
+                         const float* vowel_a, const float* vowel_b,
+                         const float* morph, const float* q_in,
+                         const float* dry_buf, float dry_const,
+                         const float* wet_buf, float wet_const,
+                         float* out_l, float* out_r) noexcept {
+    __m256 z1 = _mm256_set_ps(0.0f, state.bp3_z1[1], state.bp2_z1[1], state.bp1_z1[1],
+                              0.0f, state.bp3_z1[0], state.bp2_z1[0], state.bp1_z1[0]);
+    __m256 z2 = _mm256_set_ps(0.0f, state.bp3_z2[1], state.bp2_z2[1], state.bp1_z2[1],
+                              0.0f, state.bp3_z2[0], state.bp2_z2[0], state.bp1_z2[0]);
+
+    // Pass 1+2 (state-independent, 8 samples at a time): formant targets are a pure
+    // function of the clamped params, so recomputing them per sample equals the
+    // scalar "recompute on change" cache. Produces formant freqs, gains, 1/q.
+    alignas(32) float freq[3][BLOCK_SIZE];
+    alignas(32) float gain[3][BLOCK_SIZE];
+    alignas(32) float qinv[BLOCK_SIZE];
+
+    __m256 tab_f[3], tab_g[3];
+    tab_f[0] = _mm256_setr_ps(VOWEL_TABLE[0].f1, VOWEL_TABLE[1].f1, VOWEL_TABLE[2].f1, VOWEL_TABLE[3].f1, VOWEL_TABLE[4].f1, 0.0f, 0.0f, 0.0f);
+    tab_f[1] = _mm256_setr_ps(VOWEL_TABLE[0].f2, VOWEL_TABLE[1].f2, VOWEL_TABLE[2].f2, VOWEL_TABLE[3].f2, VOWEL_TABLE[4].f2, 0.0f, 0.0f, 0.0f);
+    tab_f[2] = _mm256_setr_ps(VOWEL_TABLE[0].f3, VOWEL_TABLE[1].f3, VOWEL_TABLE[2].f3, VOWEL_TABLE[3].f3, VOWEL_TABLE[4].f3, 0.0f, 0.0f, 0.0f);
+    tab_g[0] = _mm256_setr_ps(VOWEL_TABLE[0].g1, VOWEL_TABLE[1].g1, VOWEL_TABLE[2].g1, VOWEL_TABLE[3].g1, VOWEL_TABLE[4].g1, 0.0f, 0.0f, 0.0f);
+    tab_g[1] = _mm256_setr_ps(VOWEL_TABLE[0].g2, VOWEL_TABLE[1].g2, VOWEL_TABLE[2].g2, VOWEL_TABLE[3].g2, VOWEL_TABLE[4].g2, 0.0f, 0.0f, 0.0f);
+    tab_g[2] = _mm256_setr_ps(VOWEL_TABLE[0].g3, VOWEL_TABLE[1].g3, VOWEL_TABLE[2].g3, VOWEL_TABLE[3].g3, VOWEL_TABLE[4].g3, 0.0f, 0.0f, 0.0f);
+
+    const __m256 one = _mm256_set1_ps(1.0f);
+    const __m256 zero = _mm256_setzero_ps();
+    const __m256i four = _mm256_set1_epi32(4);
+    const __m256i one_i = _mm256_set1_epi32(1);
+
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 va = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(vowel_a + i), zero), _mm256_set1_ps(4.0f));
+        const __m256 vb = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(vowel_b + i), zero), _mm256_set1_ps(4.0f));
+        const __m256 m = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(morph + i), zero), one);
+        const __m256 q = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(q_in + i), one), _mm256_set1_ps(20.0f));
+
+        const __m256i ia = _mm256_min_epi32(_mm256_cvttps_epi32(va), four);
+        const __m256i ib = _mm256_min_epi32(_mm256_cvttps_epi32(vb), four);
+        const __m256 fa = _mm256_sub_ps(va, _mm256_cvtepi32_ps(_mm256_cvttps_epi32(va)));
+        const __m256 fb = _mm256_sub_ps(vb, _mm256_cvtepi32_ps(_mm256_cvttps_epi32(vb)));
+        const __m256i ia2 = _mm256_min_epi32(_mm256_add_epi32(ia, one_i), four);
+        const __m256i ib2 = _mm256_min_epi32(_mm256_add_epi32(ib, one_i), four);
+        const __m256 omfa = _mm256_sub_ps(one, fa);
+        const __m256 omfb = _mm256_sub_ps(one, fb);
+        const __m256 omm = _mm256_sub_ps(one, m);
+
+        for (int k = 0; k < 3; ++k) {
+            const __m256 f_a = formant_lerp_avx2(tab_f[k], ia, ia2, omfa, fa);
+            const __m256 f_b = formant_lerp_avx2(tab_f[k], ib, ib2, omfb, fb);
+            const __m256 f = _mm256_add_ps(_mm256_mul_ps(f_a, omm), _mm256_mul_ps(f_b, m));
+            _mm256_store_ps(freq[k] + i, f);
+
+            const __m256 g_a = formant_lerp_avx2(tab_g[k], ia, ia2, omfa, fa);
+            const __m256 g_b = formant_lerp_avx2(tab_g[k], ib, ib2, omfb, fb);
+            _mm256_store_ps(gain[k] + i, _mm256_add_ps(_mm256_mul_ps(g_a, omm), _mm256_mul_ps(g_b, m)));
+        }
+        _mm256_store_ps(qinv + i, _mm256_div_ps(one, q));
+    }
+
+    // Persist the scalar cache so the fallback path / next block stay consistent.
+    constexpr std::size_t L = BLOCK_SIZE - 1;
+    state.last_vowel_a = std::clamp(vowel_a[L], 0.0f, 4.0f);
+    state.last_vowel_b = std::clamp(vowel_b[L], 0.0f, 4.0f);
+    state.last_morph = std::clamp(morph[L], 0.0f, 1.0f);
+    state.last_q = std::clamp(q_in[L], 1.0f, 20.0f);
+    state.f1 = freq[0][L]; state.f2 = freq[1][L]; state.f3 = freq[2][L];
+    state.g1 = gain[0][L]; state.g2 = gain[1][L]; state.g3 = gain[2][L];
+
+    // Pass 3: the recurrence. The exact std::tan calls (state-independent) sit in
+    // the same loop so out-of-order execution overlaps them with the SVF chain.
+    // (std::tan must stay exact: the filter amplifies 1-ulp coefficient errors.)
+    auto prewarp_coef = [sample_rate](float f) -> float {
+        float w = PI * std::min(f, sample_rate * 0.48f) / sample_rate;
+        return std::min(1.9f, 2.0f * std::tan(w));
+    };
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        const float dry = drywet::coeff(dry_buf, i, dry_const);
+        const float wet = drywet::coeff(wet_buf, i, wet_const);
+        const float c1 = prewarp_coef(freq[0][i]);
+        const float c2 = prewarp_coef(freq[1][i]);
+        const float c3 = prewarp_coef(freq[2][i]);
+        const __m256 fv = _mm256_set_ps(0.0f, c3, c2, c1, 0.0f, c3, c2, c1);
+        const __m256 gv = _mm256_set_ps(0.0f, gain[2][i], gain[1][i], gain[0][i],
+                                        0.0f, gain[2][i], gain[1][i], gain[0][i]);
+        const __m256 qv = _mm256_set1_ps(qinv[i]);
+
+        const float xl = input[i];
+        const float xr = input_r ? input_r[i] : xl;
+        const __m256 xv = _mm256_insertf128_ps(
+            _mm256_castps128_ps256(_mm_set1_ps(xl)), _mm_set1_ps(xr), 1);
+
+        __m256 hp = _mm256_sub_ps(_mm256_sub_ps(xv, _mm256_mul_ps(z1, qv)), z2);
+        __m256 bp = _mm256_add_ps(z1, _mm256_mul_ps(fv, hp));
+        __m256 lp = _mm256_add_ps(z2, _mm256_mul_ps(fv, z1));
+        z1 = formant_clamp_avx2(bp);
+        z2 = formant_clamp_avx2(lp);
+
+        // (bp1*g1 + bp2*g2) + bp3*g3 per half, result in lanes 0 and 4.
+        const __m256 p = _mm256_mul_ps(bp, gv);
+        const __m256 s = _mm256_add_ps(p, _mm256_permute_ps(p, _MM_SHUFFLE(3, 3, 3, 1)));
+        const __m256 t = _mm256_add_ps(s, _mm256_permute_ps(p, _MM_SHUFFLE(3, 3, 3, 2)));
+        const float pl = _mm_cvtss_f32(_mm256_castps256_ps128(t));
+ 
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


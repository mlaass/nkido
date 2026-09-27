You are optimising Cedar DSP opcodes with SIMD. Cedar is the real-time audio
engine of nkido: a bytecode VM that processes blocks of 128 float samples at
48 kHz, zero allocations on the audio path. The repository is your current
working directory.

## Task

Make these opcodes faster on **x86-64 AVX2** (the gate machine):

- `op_filter_svf_lp`
- `op_filter_svf_hp`
- `op_filter_svf_bp`

They live in `cedar/include/cedar/opcodes/filters.hpp`. Current source of the opcode bodies:

```cpp
[[gnu::always_inline]]
inline void op_filter_svf_lp(ExecutionContext& ctx, const Instruction& inst) {
    [[maybe_unused]] constexpr int SVF_MODE = 0;
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);
#if CEDAR_SIMD_X86
    if constexpr (BLOCK_SIZE % 8 == 0) {
        if (simd::has(simd::Isa::Avx2)) {
            kernels::svf_avx2<SVF_MODE>(state, out_l, out_r, input, input_r, freq, q,
                                        dry_level, wet_level, ctx.sample_rate);
            return;
        }
    }
#endif

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_svf(state, freq[i], q[i], ctx.sample_rate);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            float v3 = x - (state.ic2eq[ch] + DENORMAL_DC);
            float v1 = state.a1 * (state.ic1eq[ch] + DENORMAL_DC) + state.a2 * v3;
            float v2 = (state.ic2eq[ch] + DENORMAL_DC) + state.a2 * (state.ic1eq[ch] + DENORMAL_DC) + state.a3 * v3;
            state.ic1eq[ch] = clamp_audio(2.0f * v1 - state.ic1eq[ch]);
            state.ic2eq[ch] = clamp_audio(2.0f * v2 - state.ic2eq[ch]);
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, v2, dry, wet);
        }
    }
}

[[gnu::always_inline]]
inline void op_filter_svf_hp(ExecutionContext& ctx, const Instruction& inst) {
    [[maybe_unused]] constexpr int SVF_MODE = 1;
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);
#if CEDAR_SIMD_X86
    if constexpr (BLOCK_SIZE % 8 == 0) {
        if (simd::has(simd::Isa::Avx2)) {
            kernels::svf_avx2<SVF_MODE>(state, out_l, out_r, input, input_r, freq, q,
                                        dry_level, wet_level, ctx.sample_rate);
            return;
        }
    }
#endif

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_svf(state, freq[i], q[i], ctx.sample_rate);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            float v3 = x - (state.ic2eq[ch] + DENORMAL_DC);
            float v1 = state.a1 * (state.ic1eq[ch] + DENORMAL_DC) + state.a2 * v3;
            float v2 = (state.ic2eq[ch] + DENORMAL_DC) + state.a2 * (state.ic1eq[ch] + DENORMAL_DC) + state.a3 * v3;
            state.ic1eq[ch] = clamp_audio(2.0f * v1 - state.ic1eq[ch]);
            state.ic2eq[ch] = clamp_audio(2.0f * v2 - state.ic2eq[ch]);
            // Highpass = input - k*bandpass - lowpass
            float hp = x - state.k * v1 - v2;
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, hp, dry, wet);
        }
    }
}

[[gnu::always_inline]]
inline void op_filter_svf_bp(ExecutionContext& ctx, const Instruction& inst) {
    [[maybe_unused]] constexpr int SVF_MODE = 2;
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* q = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);
#if CEDAR_SIMD_X86
    if constexpr (BLOCK_SIZE % 8 == 0) {
        if (simd::has(simd::Isa::Avx2)) {
            kernels::svf_avx2<SVF_MODE>(state, out_l, out_r, input, input_r, freq, q,
                                        dry_level, wet_level, ctx.sample_rate);
            return;
        }
    }
#endif

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        calc_svf(state, freq[i], q[i], ctx.sample_rate);
        float dry = drywet::coeff(dry_level, i, 0.0f);
        float wet = drywet::coeff(wet_level, i, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);
            float v3 = x - (state.ic2eq[ch] + DENORMAL_DC);
            float v1 = state.a1 * (state.ic1eq[ch] + DENORMAL_DC) + state.a2 * v3;
            float v2 = (state.ic2eq[ch] + DENORMAL_DC) + state.a2 * (state.ic1eq[ch] + DENORMAL_DC) + state.a3 * v3;
            state.ic1eq[ch] = clamp_audio(2.0f * v1 - state.ic1eq[ch]);
            state.ic2eq[ch] = clamp_audio(2.0f * v2 - state.ic2eq[ch]);
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, v1, dry, wet);
        }
    }
}
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_filter_svf_lp` | 2256.17 | 642.58 | 3.51x |
| `op_filter_svf_hp` | 2354.72 | 671.39 | 3.51x |
| `op_filter_svf_bp` | 2240.25 | 644.95 | 3.47x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..8536d86 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -5,6 +5,7 @@
 #include "../dsp/constants.hpp"
 #include "dsp_state.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
@@ -43,9 +44,200 @@ inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
     state.a3 = state.g * state.a2;
 }
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline __m256 svf_splat(float v) noexcept { return _mm256_set1_ps(v); }
+
+// tanf for float x in (2^-13, 3pi/4): a lane-wise transcription of the
+// fdlibm/glibc float algorithm (rem_pio2f for n = 1, then __kernel_tanf),
+// so results track std::tan's own rounding.
+// simd: approx — replaces std::tan in calc_svf.
+CEDAR_TARGET_AVX2 inline __m256 svf_tanf_ps(__m256 xin) noexcept {
+    const __m256i ixin = _mm256_castps_si256(xin);
+    // |x| > pi/4 (0x3f490fda): x - pi/2 with 24+24 bit pi/2, iy = -1.
+    const __m256 caseb = _mm256_castsi256_ps(_mm256_cmpgt_epi32(ixin, _mm256_set1_epi32(0x3f490fda)));
+    const __m256 zr = _mm256_sub_ps(xin, _mm256_set1_ps(1.5707855225e+00f));
+    const __m256 pio2_1t = _mm256_set1_ps(1.0804334124e-05f);
+    const __m256 y0 = _mm256_sub_ps(zr, pio2_1t);
+    const __m256 y1 = _mm256_sub_ps(_mm256_sub_ps(zr, y0), pio2_1t);
+    const __m256 x = _mm256_blendv_ps(xin, y0, caseb);
+    const __m256 y = _mm256_and_ps(y1, caseb);
+    const __m256 iy = _mm256_blendv_ps(_mm256_set1_ps(1.0f), _mm256_set1_ps(-1.0f), caseb);
+
+    const __m256 signmask = _mm256_set1_ps(-0.0f);
+    const __m256 neg = _mm256_castsi256_ps(_mm256_srai_epi32(_mm256_castps_si256(x), 31));
+    const __m256i ix = _mm256_castps_si256(_mm256_andnot_ps(signmask, x));
+    const __m256 big = _mm256_castsi256_ps(_mm256_cmpgt_epi32(ix, _mm256_set1_epi32(0x3f2ca13f)));
+    // |x| >= 0.6744: x = (pio4 - |x|) + (pio4lo - sign*y)
+    const __m256 xa = _mm256_andnot_ps(signmask, x);
+    const __m256 ya = _mm256_xor_ps(y, _mm256_and_ps(neg, signmask));
+    const __m256 xb = _mm256_add_ps(_mm256_sub_ps(_mm256_set1_ps(7.8539812565e-01f), xa),
+                                    _mm256_sub_ps(_mm256_set1_ps(3.7748947079e-08f), ya));
+    const __m256 X = _mm256_blendv_ps(x, xb, big);
+    const __m256 Y = _mm256_andnot_ps(big, y);
+
+    const __m256 z = _mm256_mul_ps(X, X);
+    const __m256 w = _mm256_mul_ps(z, z);
+    constexpr auto c = svf_splat;
+    __m256 r = _mm256_add_ps(c(7.8179444245e-05f), _mm256_mul_ps(w, c(-1.8558637748e-05f)));
+    r = _mm256_add_ps(c(5.8804126456e-04f), _mm256_mul_ps(w, r));
+    r = _mm256_add_ps(c(3.5920790397e-03f), _mm256_mul_ps(w, r));
+    r = _mm256_add_ps(c(2.1869488060e-02f), _mm256_mul_ps(w, r));
+    r = _mm256_add_ps(c(1.3333334029e-01f), _mm256_mul_ps(w, r));
+    __m256 v = _mm256_add_ps(c(7.1407252108e-05f), _mm256_mul_ps(w, c(2.5907305826e-05f)));
+    v = _mm256_add_ps(c(2.4646313977e-04f), _mm256_mul_ps(w, v));
+    v = _mm256_add_ps(c(1.4562094584e-03f), _mm256_mul_ps(w, v));
+    v = _mm256_add_ps(c(8.8632395491e-03f), _mm256_mul_ps(w, v));
+    v = _mm256_add_ps(c(5.3968254477e-02f), _mm256_mul_ps(w, v));
+    v = _mm256_mul_ps(z, v);
+    const __m256 s = _mm256_mul_ps(z, X);
+    r = _mm256_add_ps(Y, _mm256_mul_ps(z, _mm256_add_ps(_mm256_mul_ps(s, _mm256_add_ps(r, v)), Y)));
+    r = _mm256_add_ps(r, _mm256_mul_ps(c(3.3333334327e-01f), s));
+    const __m256 W = _mm256_add_ps(X, r);
+
+    // The two divide-bearing tails below are skipped when no lane needs them
+    // (typical for audio-range cutoffs well below sample_rate / 4).
+    __m256 res = W;
+    if (_mm256_movemask_ps(_mm256_andnot_ps(big, caseb)) != 0) {
+        // small, iy = -1: -1/(X+r) computed accurately
+        const __m256 hmask = _mm256_castsi256_ps(_mm256_set1_epi32(static_cast<int>(0xfffff000u)));
+        const __m256 zt = _mm256_and_ps(W, hmask);
+        const __m256 vt = _mm256_sub_ps(r, _mm256_sub_ps(zt, X));
+        const __m256 a = _mm256_div_ps(c(-1.0f), W);
+        const __m256 t = _mm256_and_ps(a, hmask);
+        const __m256 st = _mm256_add_ps(c(1.0f), _mm256_mul_ps(t, zt));
+        const __m256 rm = _mm256_add_ps(t, _mm256_mul_ps(a, _mm256_add_ps(st, _mm256_mul_ps(t, vt))));
+        res = _mm256_blendv_ps(W, rm, caseb);
+    }
+    if (_mm256_movemask_ps(big) != 0) {
+        // big: sgn * (iy - 2*(X - (W*W/(W+iy) - r)))
+        const __m256 sgn = _mm256_blendv_ps(c(1.0f), c(-1.0f), neg);
+        __m256 rb = _mm256_sub_ps(_mm256_div_ps(_mm256_mul_ps(W, W), _mm256_add_ps(W, iy)), r);
+        rb = _mm256_mul_ps(sgn, _mm256_sub_ps(iy, _mm256_mul_ps(c(2.0f), _mm256_sub_ps(X, rb))));
+        // big and |X| < 2^-13: sgn*iy * (1 - 2*iy*X)
+        const __m256 tiny = _mm256_cmp_ps(_mm256_andnot_ps(signmask, xb), c(0x1p-13f), _CMP_LT_OQ);
+        const __m256 rt = _mm256_mul_ps(_mm256_mul_ps(sgn, iy),
+                                        _mm256_sub_ps(c(1.0f), _mm256_mul_ps(_mm256_mul_ps(c(2.0f), iy), xb)));
+        rb = _mm256_blendv_ps(rb, rt, tiny);
+        res = _mm256_blendv_ps(res, rb, big);
+    }
+    return res;
+}
+
+CEDAR_TARGET_AVX2 inline __m128 svf_clamp_ps(__m128 v) noexcept {
+    const __m128 c = _mm_min_ps(_mm_max_ps(v, _mm_set1_ps(-10.0f)), _mm_set1_ps(10.0f));
+    return _mm_and_ps(c, _mm_cmpord_ps(v, v));  // NaN -> 0
+}
+
+// Mode: 0 = lowpass (v2), 1 = highpass, 2 = bandpass (v1).
+// Coefficients are a pure function of (freq, q), so they are computed for the
+// whole block first; only the integrator recurrence stays serial.
+template <int Mode>
+CEDAR_TARGET_AVX2 inline void svf_avx2(SVFState& state, float* out_l, float* out_r,
+                                       const float* in_l, const float* in_r,
+                                       const float* freq, const float* q,
+                                       const float* dry_level, const float* wet_level,
+                                       float sample_rate) noexcept {
+    alignas(32) float A1[BLOCK_SIZE], A2[BLOCK_SIZE], A3[BLOCK_SIZE], K[BLOCK_SIZE], G[BLOCK_SIZE];
+    alignas(32) float F0[BLOCK_SIZE];
+    alignas(32) float Y[2 * BLOCK_SIZE];
+
+    const __m256 hi = _mm256_set1_ps(sample_rate * 0.49f);
+    const __m256 lo = _mm256_set1_ps(20.0f);
+    const __m256 zero = _mm256_setzero_ps();
+    const __m256 one = _mm256_set1_ps(1.0f);
+    const __m256 qmin = _mm256_set1_ps(0.1f);
+    const __m256 pi = _mm256_set1_ps(PI);
+    const __m256 sr = _mm256_set1_ps(sample_rate);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 f = _mm256_load_ps(freq + i);
+        const __m256 f0 = _mm256_and_ps(_mm256_cmp_ps(zero, f, _CMP_LT_OQ), f);  // max(0, f)
+        __m256 fc = _mm256_blendv_ps(f0, hi, _mm256_cmp_ps(hi, f0, _CMP_LT_OQ));
+        fc = _mm256_blendv_ps(fc, lo, _mm256_cmp_ps(f0, lo, _CMP_LT_OQ));
+        const __m256 qv = _mm256_load_ps(q + i);
+        const __m256 qc = _mm256_blendv_ps(qmin, qv, _mm256_cmp_ps(qmin, qv, _CMP_LT_OQ));
+        const __m256 x = _mm256_div_ps(_mm256_mul_ps(pi, fc), sr);
+        const __m256 g = svf_tanf_ps(x);
+        const __m256 k = _mm256_div_ps(one, qc);
+        const __m256 a1 = _mm256_div_ps(one, _mm256_add_ps(one, _mm256_mul_ps(g, _mm256_add_ps(g, k))));
+        const __m256 a2 = _mm256_mul_ps(g, a1);
+        const __m256 a3 = _mm256_mul_ps(g, a2);
+        _mm256_store_ps(F0 + i, f0);
+        _mm256_store_ps(G + i, g);
+        _mm256_store_ps(K + i, k);
+        _mm256_store_ps(A1 + i, a1);
+        _mm256_store_ps(A2 + i, a2);
+        _mm256_store_ps(A3 + i, a3);
+    }
+    // Honour the scalar coefficient cache for a leading run of unchanged params.
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        if (!(F0[i] == state.last_freq && q[i] == state.last_q)) break;
+        G[i] = state.g; K[i] = state.k; A1[i] = state.a1; A2[i] = state.
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
   Exception: op_filter_svf_lp, op_filter_svf_hp, op_filter_svf_bp call `tanh`/`tan`; for these only, an approximation is allowed if (a) you mark it with a `// simd: approx` comment naming the replaced function, and (b) its peak error stays at or below -100 dBFS (about 1e-5 absolute) on every stimulus.
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-25T1939Z_opus_svf/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | per-sample calc_svf (scalar tanf + 2 divides) serialised with the recurrence -> precompute all coefficients 8-wide with a lane-wise glibc-faithful tanf, then run only the serial recurrence with L/R in | filter_svf_lp 3.766x, filter_svf_hp 2.962x, filter_svf_bp 3.179x |
| 2 | rejected: `speed` — slower than baseline: op_filter_svf_hp 0.937x | serial integrator recurrence (6 dependent ops/sample) -> pre-doubled coefficients fold 2·v into the products for a 5-op chain, with a block-wide range guard and a cold exact fallback for subnormal/ove | — |
| 3 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_filter_svf_lp 1.033x (band 4.8%), op_filter_svf_hp 0.964x (band  | coefficient pass (tanf + divides) serialised before the latency-bound recurrence -> software-pipeline it, computing chunk c+8's coefficients while chunk c's 8 recurrence steps run, plus exact v+v for  | — |
| 4 | ACCEPTED | coefficient pass bound by vector divides in the lane-wise tanf (both the reduced-range -1/(X+r) and big-/x/ W²/(W+iy) tails were always computed and blended away) -> skip each divide-bearing tail with | filter_svf_lp 1.148x, filter_svf_hp 1.138x, filter_svf_bp 1.147x |

Full verifier output for the latest rejection (attempt 3, gate `speed`):
```
no opcode improved by >= 5 % outside the noise band: op_filter_svf_lp 1.033x (band 4.8%), op_filter_svf_hp 0.964x (band 26.8%), op_filter_svf_bp 0.968x (band 15.2%)
```


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
#if CEDAR_SIMD_X86
    if (kernels::svf_try_avx2<kernels::SvfLP>(ctx, inst)) return;
#endif
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
#if CEDAR_SIMD_X86
    if (kernels::svf_try_avx2<kernels::SvfHP>(ctx, inst)) return;
#endif
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
#if CEDAR_SIMD_X86
    if (kernels::svf_try_avx2<kernels::SvfBP>(ctx, inst)) return;
#endif
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
| `op_filter_svf_lp` | 2256.17 | 287.73 | 7.84x |
| `op_filter_svf_hp` | 2354.72 | 299.39 | 7.87x |
| `op_filter_svf_bp` | 2240.25 | 284.5 | 7.87x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/dsp/simd.hpp b/cedar/include/cedar/dsp/simd.hpp
index ee273e4..e90cea1 100644
--- a/cedar/include/cedar/dsp/simd.hpp
+++ b/cedar/include/cedar/dsp/simd.hpp
@@ -24,6 +24,8 @@
 #  define CEDAR_SIMD_X86 1
 #  include <immintrin.h>
 #  define CEDAR_TARGET_AVX2 [[gnu::target("avx2")]]
+// Only for kernels that tolerate fused rounding; Isa::Avx2 detection requires FMA as well.
+#  define CEDAR_TARGET_AVX2_FMA [[gnu::target("avx2,fma")]]
 #else
 #  define CEDAR_SIMD_X86 0
 #endif
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..fe1f9f9 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -3,6 +3,7 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include "dsp_state.hpp"
 #include "drywet.hpp"
 #include <cmath>
@@ -43,9 +44,480 @@ inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
     state.a3 = state.g * state.a2;
 }
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+enum SvfMode { SvfLP = 0, SvfHP = 1, SvfBP = 2 };
+
+// simd: approx — replaces std::tan (Cephes tanf polynomial, ~1e-7 relative error).
+// Input range is (0, pi/2); above pi/4 uses tan(x) = 1 / tan(pi/2 - x).
+CEDAR_TARGET_AVX2_FMA inline __m256 svf_tan_avx2(__m256 x) noexcept {
+    const __m256 big = _mm256_cmp_ps(x, _mm256_set1_ps(0.78539816339744831f), _CMP_GT_OQ);
+    const __m256 red = _mm256_add_ps(
+        _mm256_sub_ps(_mm256_set1_ps(1.57079637050628662f), x),
+        _mm256_set1_ps(-4.3711388e-8f));
+    const __m256 y = _mm256_blendv_ps(x, red, big);
+    const __m256 z = _mm256_mul_ps(y, y);
+    __m256 p = _mm256_set1_ps(9.38540185543e-3f);
+    p = _mm256_fmadd_ps(p, z, _mm256_set1_ps(3.11992232697e-3f));
+    p = _mm256_fmadd_ps(p, z, _mm256_set1_ps(2.44301354525e-2f));
+    p = _mm256_fmadd_ps(p, z, _mm256_set1_ps(5.34112807005e-2f));
+    p = _mm256_fmadd_ps(p, z, _mm256_set1_ps(1.33387994085e-1f));
+    p = _mm256_fmadd_ps(p, z, _mm256_set1_ps(3.33331568548e-1f));
+    const __m256 t = _mm256_fmadd_ps(_mm256_mul_ps(p, z), y, y);
+    if (_mm256_movemask_ps(big) == 0) return t;  // no lane above pi/4: skip the reciprocal
+    return _mm256_blendv_ps(t, _mm256_div_ps(_mm256_set1_ps(1.0f), t), big);
+}
+
+// Serial state recurrence. Clamp=true applies clamp_audio to the state each sample.
+// Clamp=false skips it and returns true if any state value would have been changed by it
+// (|n| > 10 or NaN), in which case the results are to be discarded and recomputed.
+template <int Mode, bool Clamp>
+CEDAR_TARGET_AVX2 inline bool svf_recur_avx2(const float* in_l, const float* in_r,
+                                             const float* m11, const float* A2,
+                                             const float* m22, const float* A3,
+                                             float* p1l, float* p1r, float* p2l, float* p2r,
+                                             __m128& ic1_io, __m128& ic2_io) noexcept {
+    const __m128 dc2 = _mm_set1_ps(2.0f * DENORMAL_DC);
+    const __m128 half = _mm_set1_ps(0.5f);
+    const __m128 lim_hi = _mm_set1_ps(10.0f);
+    [[maybe_unused]] const __m128 lim_lo = _mm_set1_ps(-10.0f);
+    const __m128 absmask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
+    __m128 ic1 = ic1_io;
+    __m128 ic2 = ic2_io;
+    __m128 bad = _mm_setzero_ps();
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        const __m128 x = _mm_unpacklo_ps(_mm_load_ss(in_l + i), _mm_load_ss(in_r + i));
+        const __m128 c11 = _mm_set1_ps(m11[i]);
+        const __m128 cA2 = _mm_set1_ps(A2[i]);
+        const __m128 c22 = _mm_set1_ps(m22[i]);
+        const __m128 cA3 = _mm_set1_ps(A3[i]);
+        // Input terms (off the recurrence's critical path); dc2 keeps the state out of denormals.
+        const __m128 b1 = _mm_add_ps(_mm_mul_ps(cA2, x), dc2);
+        const __m128 b2 = _mm_add_ps(_mm_mul_ps(cA3, x), dc2);
+        const __m128 n1 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(c11, ic1), _mm_mul_ps(cA2, ic2)), b1);
+        const __m128 n2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(cA2, ic1), _mm_mul_ps(c22, ic2)), b2);
+        // n = 2*v - ic  =>  v = (n + ic) / 2
+        const __m128 v1 = _mm_mul_ps(half, _mm_add_ps(n1, ic1));
+        const __m128 v2 = _mm_mul_ps(half, _mm_add_ps(n2, ic2));
+        if constexpr (Clamp) {
+            // clamp_audio: NaN -> 0, else clamp to +-10
+            ic1 = _mm_and_ps(_mm_min_ps(_mm_max_ps(n1, lim_lo), lim_hi), _mm_cmpord_ps(n1, n1));
+            ic2 = _mm_and_ps(_mm_min_ps(_mm_max_ps(n2, lim_lo), lim_hi), _mm_cmpord_ps(n2, n2));
+        } else {
+            // not(|n| <= 10) is also true for NaN
+            bad = _mm_or_ps(bad, _mm_or_ps(_mm_cmpnle_ps(_mm_and_ps(n1, absmask), lim_hi),
+                                           _mm_cmpnle_ps(_mm_and_ps(n2, absmask), lim_hi)));
+            ic1 = n1;
+            ic2 = n2;
+        }
+        if constexpr (Mode != SvfLP) {
+            p1l[i] = _mm_cvtss_f32(v1);
+            p1r[i] = _mm_cvtss_f32(_mm_shuffle_ps(v1, v1, 1));
+        }
+        if constexpr (Mode != SvfBP) {
+            p2l[i] = _mm_cvtss_f32(v2);
+            p2r[i] = _mm_cvtss_f32(_mm_shuffle_ps(v2, v2, 1));
+        }
+    }
+    ic1_io = ic1;
+    ic2_io = ic2;
+    return (_mm_movemask_ps(bad) & 3) != 0;
+}
+
+// Slot of sample j (0..7 of a group of eight) in the L/R-pair layout: pairs of samples come out
+// of _mm256_unpack{lo,hi}_ps as [s0 s1 s4 s5] [s2 s3 s6 s7], and _mm256_shuffle_ps(a, b, 0x88/0xDD)
+// turns that back into sample order.
+constexpr int svf_pos(int j) noexcept { return (j & 1) | ((j & 4) >> 1) | ((j & 2) << 1); }
+
+CEDAR_TARGET_AVX2 inline void svf_deint_avx2(const float* p, __m256& l, __m256& r) noexcept {
+    const __m256 a = _mm256_load_ps(p);
+    const __m256 b = _mm256_load_ps(p + 8);
+    l = _mm256_shuffle_ps(a, b, 0x88);
+    r = _mm256_shuffle_ps(a, b, 0xDD);
+}
+
+// Two-step lookahead recurrence (unclamped, speculative). State is one register
+// s = [ic1L, ic1R, ic2L, ic2R]; one step is s' = D1*s + D2*swap(s) + b. The even-sample chain
+// advances two samples at a time with the precomputed composite (P, c) so its dependency
+// latency is halved per sample; the odd-sample state is derived from the even one off-chain.
+// Outputs are 2*v = n + ic stored as L/R pairs (v1 -> p1, v2 -> p2). Returns true if any state
+// value left +-10 or went NaN (then the caller redoes the block with the exact clamp).
+template <int Mode>
+CEDAR_TARGET_AVX2_FMA inline bool svf_recur2_avx2(const float* xlr, const float* m11, const float* A2,
+                                              const float* m22, const float* A3,
+                                              const float* P11, const float* P12,
+                                              const float* P21, const float* P22,
+                                              const float* C1, const float* C2,
+                                              float* p1, float* p2, __m128& s_io) noexcept {
+    const __m128 dc2 = _mm_set1_ps(2.0f * DENORMAL_DC);
+    const __m128 neg_lo = _mm_setr_ps(-0.0f, -0.0f, 0.0f, 0.0f);
+    const __m128 absmask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
+    const __m128 lim_hi = _mm_set1_ps(10.0f);
+    __m128 s = s_io;
+    __m128 mx = _mm_setzero_ps();
+    for (std::size_t g = 0; g < BLOCK_SIZE / 8; ++g) {
+#pragma GCC unroll 4
+        for (int j = 0; j < 8; j += 2) {
+            const std::size_t i = g * 8 + static_cast<std::size_t>(j);
+            const std::size_t o = g * 16 + 2 * static_cast<std::size_t>(svf_pos(j));
+            const __m128 e = _mm_set1_ps(A2[i]);
+            const __m128 D1 = _mm_blend_ps(_mm_set1_ps(m11[i]), _mm_set1_ps(m22[i]), 0xC);
+            const __m128 D2 = _mm_xor_ps(e, neg_lo);
+            const __m128 A23 = _mm_blend_ps(e, _mm_set1_ps(A3[i]), 0xC);
+            const __m128 D1p = _mm_blend_ps(
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1247Z_sonnet_svf/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | per-sample `std::tan` and divisions in `calc_svf` (freq/q ramp every sample) -> precompute all 128 samples' coefficients with a vectorised Cephes tanf approximation, run the L/R recurrence as one SSE  | filter_svf_lp 2.649x, filter_svf_hp 2.669x, filter_svf_bp 2.585x |
| 2 | ACCEPTED | serial L/R state recurrence latency (~33 cycles/sample) -> fold the update into a precomputed 2x2 transition (n = M·ic + b) with v=(n+ic)/2 and DC folded into b, cutting the critical path to ~21 cycle | filter_svf_lp 1.226x, filter_svf_hp 1.224x, filter_svf_bp 1.252x |
| 3 | ACCEPTED | serial L/R state recurrence latency (~21 cycles/sample, with the clamp's max/min/and on the critical path) -> run the recurrence unclamped, flag /state/>10/NaN off the critical path, and re-run the bl | filter_svf_lp 1.49x, filter_svf_hp 1.482x, filter_svf_bp 1.5x |
| 4 | ACCEPTED | serial L/R state recurrence latency (~12 cycles/sample, port-saturated in the 2-lane form) -> two-step lookahead with a precomputed composite transition (P=M(i+1)M(i), c) on a 4-lane [ic1L,ic1R,ic2L,i | filter_svf_lp 1.299x, filter_svf_hp 1.31x, filter_svf_bp 1.282x |
| 5 | ACCEPTED | serial two-step recurrence chain (shuffle+mul+add+add, ~12 cycles per 2 samples) -> fused the chain with `_mm_fmadd_ps` (2 FMAs deep, ~8 cycles) in an avx2+fma-targeted recurrence and made `Isa::Avx2` | filter_svf_lp 1.07x, filter_svf_hp 1.082x, filter_svf_bp 1.069x |
| 6 | ACCEPTED | uop-throughput-bound coefficient/mix passes (separate mul+add everywhere; the constant-freq/q fast path gave nothing, so the benchmark sweeps params) -> FMA-targeted `svf_tan_avx2`/`svf_avx2` with mul | filter_svf_lp 1.137x, filter_svf_hp 1.143x, filter_svf_bp 1.153x |


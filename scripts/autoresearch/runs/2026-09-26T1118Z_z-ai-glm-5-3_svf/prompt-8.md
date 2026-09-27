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
    if (simd::has(simd::Isa::Avx2)) {
        svf_kernels::run_avx2(state, input, stereo_in ? input_r : nullptr,
                              freq, q, dry_level, wet_level, out_l, out_r,
                              ctx.sample_rate, /*mode=*/0);
        return;
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
    if (simd::has(simd::Isa::Avx2)) {
        svf_kernels::run_avx2(state, input, stereo_in ? input_r : nullptr,
                              freq, q, dry_level, wet_level, out_l, out_r,
                              ctx.sample_rate, /*mode=*/1);
        return;
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
    if (simd::has(simd::Isa::Avx2)) {
        svf_kernels::run_avx2(state, input, stereo_in ? input_r : nullptr,
                              freq, q, dry_level, wet_level, out_l, out_r,
                              ctx.sample_rate, /*mode=*/2);
        return;
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
| `op_filter_svf_lp` | 2256.17 | 982.66 | 2.30x |
| `op_filter_svf_hp` | 2354.72 | 987.46 | 2.38x |
| `op_filter_svf_bp` | 2240.25 | 984.14 | 2.28x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..a439b1a 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -5,6 +5,7 @@
 #include "../dsp/constants.hpp"
 #include "dsp_state.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
@@ -43,6 +44,284 @@ inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
     state.a3 = state.g * state.a2;
 }
 
+// ============================================================================
+// SVF AVX2 kernels (x86-64 only).
+//
+// The scalar body spends its time on `std::tan` (every sample, for modulated
+// cutoff) plus a scalar 2-channel recurrence. The kernels keep the
+// recurrence serial (it must be — ic1eq/ic2eq feed back sample-to-sample)
+// but:
+//
+//   1. Precompute the whole block of SVF coefficients up front with 8-wide
+//      float vectors, exactly mirroring the operand grouping of `calc_svf`.
+//      IEEE float ops with the same tree shape and no FMA give bit-identical
+//      coefficients, except `tan` — see tan8_ps below.
+//   2. Run L and R of each sample in the two lanes of one __m128, with
+//      clamp_audio() expressed as NaN-mask + min/max (identical semantics).
+//
+// Denormals are never flushed: DAZ/FTZ are not touched, and every arithmetic
+// instruction used rounds exactly like the scalar SSE2 code.
+// ============================================================================
+
+#if CEDAR_SIMD_X86
+namespace svf_kernels {
+
+inline constexpr std::size_t W = 8;  // AVX2 float lane count
+
+// tan(x) for x in [0.0013, 1.5394] (= PI*freq/sr, freq clamped to
+// [20, sr*0.49]).
+// simd: approx — replaces std::tan (tanf) in the SVF coefficient path.
+// Maclaurin sin and cos series in double precision, followed by one
+// division. The argument is clamped to [0.0013, PI*0.49], so t = x*x
+// <= 2.3698; with the sin series truncated after the 1/11! term and cos
+// after the 1/12! term, the truncation errors are bounded by
+//   sin: t^6/13! < 3.1e-8 (relative to S, |S| in [~0.21, 1] here),
+//   cos: t^6/14! < 2.1e-9,
+// giving a tan error < ~1.5e-7 absolute (tan <= ~33 on this range) —
+// roughly 5 digits of margin under the 1e-5 (-100 dBFS) allowance.
+// No FMA is used (plain mul/sub), so the code is correct on any AVX2 CPU.
+CEDAR_TARGET_AVX2 inline __m256d tan4_pd(__m256d v) noexcept {
+    const __m256d t = _mm256_mul_pd(v, v);
+    // sin(v) = v * (1 - t*(1/3! - t*(1/5! - t*(1/7! - t*(1/9! - t/11!)))))
+    __m256d s = _mm256_set1_pd(1.0 / 39916800.0);   // 1/11!
+    s = _mm256_sub_pd(_mm256_set1_pd(1.0 / 362880.0), _mm256_mul_pd(t, s));                  // 1/9!
+    s = _mm256_sub_pd(_mm256_set1_pd(1.0 / 5040.0), _mm256_mul_pd(t, s));                    // 1/7!
+    s = _mm256_sub_pd(_mm256_set1_pd(1.0 / 120.0), _mm256_mul_pd(t, s));                     // 1/5!
+    s = _mm256_sub_pd(_mm256_set1_pd(1.0 / 6.0), _mm256_mul_pd(t, s));                       // 1/3!
+    s = _mm256_sub_pd(_mm256_set1_pd(1.0), _mm256_mul_pd(t, s));                             // S
+    s = _mm256_mul_pd(v, s);                                                                 // sin
+    // cos(v) = 1 - t*(1/2! - t*(1/4! - t*(1/6! - t*(1/8! - t*(1/10! - t/12!)))))
+    __m256d c = _mm256_set1_pd(1.0 / 479001600.0);  // 1/12!
+    c = _mm256_sub_pd(_mm256_set1_pd(1.0 / 3628800.0), _mm256_mul_pd(t, c));                 // 1/10!
+    c = _mm256_sub_pd(_mm256_set1_pd(1.0 / 40320.0), _mm256_mul_pd(t, c));                   // 1/8!
+    c = _mm256_sub_pd(_mm256_set1_pd(1.0 / 720.0), _mm256_mul_pd(t, c));                     // 1/6!
+    c = _mm256_sub_pd(_mm256_set1_pd(1.0 / 24.0), _mm256_mul_pd(t, c));                      // 1/4!
+    c = _mm256_sub_pd(_mm256_set1_pd(1.0 / 2.0), _mm256_mul_pd(t, c));                       // 1/2!
+    c = _mm256_sub_pd(_mm256_set1_pd(1.0), _mm256_mul_pd(t, c));                             // cos
+    return _mm256_div_pd(s, c);                                                              // tan
+}
+
+// tan for 8 float lanes at once (see tan4_pd for the error bound).
+CEDAR_TARGET_AVX2 inline __m256 tan8_ps(__m256 x) noexcept {
+    const __m256d r0 = tan4_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(x)));
+    const __m256d r1 = tan4_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(x, 1)));
+    return _mm256_set_m128(_mm256_cvtpd_ps(r1), _mm256_cvtpd_ps(r0));
+}
+
+// Per-block SVF coefficients: g, k, a1, a2, a3 for every sample, computed with
+// 8-wide vectors. Mirrors calc_svf() exactly, except that the tan/freq-change
+// cache is irrelevant (recomputing identical coefficients gives identical
+// values) and tan comes from tan8_ps.
+CEDAR_TARGET_AVX2 inline void coefs_avx2(const float* freq, const float* q,
+                                         float sample_rate,
+                                         float* g, float* k, float* a1,
+                                         float* a2, float* a3) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(freq, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(q, 32);
+    const __m256 zero = _mm256_setzero_ps();
+    const __m256 fmin = _mm256_set1_ps(20.0f);
+    const __m256 fmax = _mm256_set1_ps(sample_rate * 0.49f);
+    const __m256 qmin = _mm256_set1_ps(0.1f);
+    const __m256 piv = _mm256_set1_ps(PI);
+    const __m256 srv = _mm256_set1_ps(sample_rate);
+    const __m256 one = _mm256_set1_ps(1.0f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += W) {
+        // freq = max(0, freq); clamp(20, sr*0.49); q = max(0.1, q).
+        // maxps returns the second operand when the first is NaN — the same
+        // value std::max(0.0f, x) / std::max(0.1f, x) produce for NaN input.
+        __m256 f = _mm256_max_ps(_mm256_load_ps(freq + i), zero);
+        f = _mm256_max_ps(f, fmin);
+        f = _mm256_min_ps(f, fmax);
+        const __m256 qv = _mm256_max_ps(_mm256_load_ps(q + i), qmin);
+        // tan(PI * freq / sample_rate) — same operand grouping as the scalar.
+        const __m256 gv = tan8_ps(_mm256_div_ps(_mm256_mul_ps(piv, f), srv));
+        const __m256 kv = _mm256_div_ps(one, qv);
+        const __m256 a1v = _mm256_div_ps(one, _mm256_add_ps(one, _mm256_mul_ps(gv, _mm256_add_ps(gv, kv))));
+        const __m256 a2v = _mm256_mul_ps(gv, a1v);
+        const __m256 a3v = _mm256_mul_ps(gv, a2v);
+        _mm256_store_ps(g + i, gv);
+        _mm256_store_ps(k + i, kv);
+        _mm256_store_ps(a1 + i, a1v);
+        _mm256_store_ps(a2 + i, a2v);
+        _mm256_store_ps(a3 + i, a3v);
+    }
+}
+
+// mode: 0 = lowpass (v2), 1 = highpass (x - k*v1 - v2), 2 = bandpass (v1).
+// The two channels run in the two lanes of one __m128; every expression uses
+// the scalar body's exact grouping, so the recurrence is bit-identical
+// (including denormals — nothing here flushes them).
+CEDAR_TARGET_AVX2
+inline void run_avx2(SVFState& state,
+                                       const float* in_l, const float* in_r,
+                                       const float* freq, const float* q,
+                                       const float* dry_buf, const float* wet_buf,
+                                       float* out_l, float* out_r,
+                                       float sample_rate, int mode) noexcept {
+    CEDAR_SIMD_ASSERT_ALIGNED(out_l, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(out_r, 32);
+    // Coefficient staging: only the final float vectors (G/K/A1/A2/A3, which
+    // feed the recurrence directly, no memory round-trip) go through stack
+    // arrays. The tan argument (double precision) is computed once here for
+    // the whole block, instead of re-running the fixup chain per 8-lane group.
+    CEDAR_SIMD_ASSERT_ALIGNED(freq, 32);
+    CEDAR_SIMD_ASSERT_ALIGNED(q, 32);
+    const __m256 zero = _mm256_setzero_ps();
+    const __m256 fmin = _mm256_set1_ps(20.0f);
+    const __m256 fmax = _mm256_set1_ps(sample_rate * 0.49f);
+    const __m256 qmin 
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1118Z_z-ai-glm-5-3_svf/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | rejected: `build` — ../../../cedar/include/cedar/opcodes/filters.hpp:183:22: error: ‘a2v’ was not declared in this scope; did you mean ‘a1v’ | (no IDEA line) | — |
| 2 | rejected: `backend error` — request failed: The read operation timed out | (no IDEA line) | — |
| 3 | ACCEPTED | per-sample std::tan + scalar 2-channel recurrence (cache never hits under modulated cutoff) -> AVX2 kernel with (1) 8-wide block-precomputed coefficients mirroring calc_svf's exact float grouping, tan | filter_svf_lp 1.886x, filter_svf_hp 1.958x, filter_svf_bp 1.879x |
| 4 | ACCEPTED | serial stereo recurrence + oversized tan series (25/26-term double Maclaurin) in the coefficient precompute -> truncated the series to sin x^11 / cos x^12 (error ≤ ~1.5e-7, bounded by x ≤ π·0.49) (1.1 | filter_svf_lp 1.132x, filter_svf_hp 1.134x, filter_svf_bp 1.132x |
| 5 | ACCEPTED | (no IDEA line) | filter_svf_lp 1.17x, filter_svf_hp 1.17x, filter_svf_bp 1.17x |
| 6 | rejected: `backend error` — HTTP 402: {"error":{"message":"This request requires more credits, or fewer max_tokens. You requested up to 131072 token | (no IDEA line) | — |
| 7 | rejected: `backend error` — HTTP 402: {"error":{"message":"This request requires more credits, or fewer max_tokens. You requested up to 131072 token | (no IDEA line) | — |

Full verifier output for the latest rejection (attempt 7, gate `backend error`):
```
HTTP 402: {"error":{"message":"This request requires more credits, or fewer max_tokens. You requested up to 131072 tokens, but can only afford 6423. To increase, visit https://openrouter.ai/workspaces/default/keys/ba23ad85e7b19c1bef654151e3b35fdff7400a4cc95ce165a166200d326714b2 and adjust the key's total limit","code":402,"metadata":{"limit_source":"openrouter_credits","remedy_hint":"Add credits at https://openrouter.ai/settings/credits, or lower max_tokens / prompt size to fit your remaining balance.","provider_name":null,"previous_errors":[{"code":402,"message":"This request requires more credits, or fewer max_tokens. You requested up to 131072 tokens, but can only afford 6423. To increase, visit https://openrouter.ai/workspaces/default/keys/ba23ad85e7b19c1bef654151e3b35fdff7400a4cc95ce165a166200d
```


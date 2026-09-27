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
| `op_filter_svf_lp` | 2256.17 | 854.25 | 2.64x |
| `op_filter_svf_hp` | 2354.72 | 881.63 | 2.67x |
| `op_filter_svf_bp` | 2240.25 | 875.77 | 2.56x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..727eed1 100644
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
@@ -43,9 +44,169 @@ inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
     state.a3 = state.g * state.a2;
 }
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+enum SvfMode { SvfLP = 0, SvfHP = 1, SvfBP = 2 };
+
+// simd: approx — replaces std::tan (Cephes tanf polynomial, ~1e-7 relative error).
+// Input range is (0, pi/2); above pi/4 uses tan(x) = 1 / tan(pi/2 - x).
+CEDAR_TARGET_AVX2 inline __m256 svf_tan_avx2(__m256 x) noexcept {
+    const __m256 big = _mm256_cmp_ps(x, _mm256_set1_ps(0.78539816339744831f), _CMP_GT_OQ);
+    const __m256 red = _mm256_add_ps(
+        _mm256_sub_ps(_mm256_set1_ps(1.57079637050628662f), x),
+        _mm256_set1_ps(-4.3711388e-8f));
+    const __m256 y = _mm256_blendv_ps(x, red, big);
+    const __m256 z = _mm256_mul_ps(y, y);
+    __m256 p = _mm256_set1_ps(9.38540185543e-3f);
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(3.11992232697e-3f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(2.44301354525e-2f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(5.34112807005e-2f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(1.33387994085e-1f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(3.33331568548e-1f));
+    const __m256 t = _mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(p, z), y), y);
+    return _mm256_blendv_ps(t, _mm256_div_ps(_mm256_set1_ps(1.0f), t), big);
+}
+
+template <int Mode>
+CEDAR_TARGET_AVX2 inline void svf_avx2(const float* in_l, const float* in_r,
+                                       const float* freq, const float* q,
+                                       const float* dry_level, const float* wet_level,
+                                       float* out_l, float* out_r,
+                                       float sample_rate, SVFState& state) noexcept {
+    alignas(32) float a1[BLOCK_SIZE];
+    alignas(32) float a2[BLOCK_SIZE];
+    alignas(32) float a3[BLOCK_SIZE];
+    alignas(32) float kk[BLOCK_SIZE];
+    alignas(32) float p1l[BLOCK_SIZE];
+    alignas(32) float p1r[BLOCK_SIZE];
+    alignas(32) float p2l[BLOCK_SIZE];
+    alignas(32) float p2r[BLOCK_SIZE];
+
+    // Pass 1: per-sample coefficients (mirrors calc_svf, independent of filter state).
+    {
+        const __m256 zero = _mm256_setzero_ps();
+        const __m256 one = _mm256_set1_ps(1.0f);
+        const __m256 lo = _mm256_set1_ps(20.0f);
+        const __m256 hi = _mm256_set1_ps(sample_rate * 0.49f);
+        const __m256 pi = _mm256_set1_ps(PI);
+        const __m256 sr = _mm256_set1_ps(sample_rate);
+        const __m256 qmin = _mm256_set1_ps(0.1f);
+        for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+            __m256 f = _mm256_max_ps(_mm256_load_ps(freq + i), zero);  // NaN -> 0
+            f = _mm256_min_ps(_mm256_max_ps(f, lo), hi);
+            const __m256 g = svf_tan_avx2(_mm256_div_ps(_mm256_mul_ps(pi, f), sr));
+            const __m256 k = _mm256_div_ps(one, _mm256_max_ps(_mm256_load_ps(q + i), qmin));
+            const __m256 a1v = _mm256_div_ps(one, _mm256_add_ps(one, _mm256_mul_ps(g, _mm256_add_ps(g, k))));
+            const __m256 a2v = _mm256_mul_ps(g, a1v);
+            const __m256 a3v = _mm256_mul_ps(g, a2v);
+            _mm256_store_ps(a1 + i, a1v);
+            _mm256_store_ps(a2 + i, a2v);
+            _mm256_store_ps(a3 + i, a3v);
+            if constexpr (Mode == SvfHP) _mm256_store_ps(kk + i, k);
+        }
+    }
+
+    // Pass 2: serial recurrence, L/R as the two lanes of one SSE register.
+    {
+        const __m128 dc = _mm_set1_ps(DENORMAL_DC);
+        const __m128 two = _mm_set1_ps(2.0f);
+        const __m128 lim_hi = _mm_set1_ps(10.0f);
+        const __m128 lim_lo = _mm_set1_ps(-10.0f);
+        __m128 ic1 = _mm_setr_ps(state.ic1eq[0], state.ic1eq[1], 0.0f, 0.0f);
+        __m128 ic2 = _mm_setr_ps(state.ic2eq[0], state.ic2eq[1], 0.0f, 0.0f);
+        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+            const __m128 x = _mm_unpacklo_ps(_mm_load_ss(in_l + i), _mm_load_ss(in_r + i));
+            const __m128 c1 = _mm_set1_ps(a1[i]);
+            const __m128 c2 = _mm_set1_ps(a2[i]);
+            const __m128 c3 = _mm_set1_ps(a3[i]);
+            const __m128 ic1d = _mm_add_ps(ic1, dc);
+            const __m128 ic2d = _mm_add_ps(ic2, dc);
+            const __m128 v3 = _mm_sub_ps(x, ic2d);
+            const __m128 a2ic1 = _mm_mul_ps(c2, ic1d);
+            const __m128 v1 = _mm_add_ps(_mm_mul_ps(c1, ic1d), _mm_mul_ps(c2, v3));
+            const __m128 v2 = _mm_add_ps(_mm_add_ps(ic2d, a2ic1), _mm_mul_ps(c3, v3));
+            const __m128 n1 = _mm_sub_ps(_mm_mul_ps(two, v1), ic1);
+            const __m128 n2 = _mm_sub_ps(_mm_mul_ps(two, v2), ic2);
+            // clamp_audio: NaN -> 0, else clamp to +-10
+            ic1 = _mm_and_ps(_mm_min_ps(_mm_max_ps(n1, lim_lo), lim_hi), _mm_cmpord_ps(n1, n1));
+            ic2 = _mm_and_ps(_mm_min_ps(_mm_max_ps(n2, lim_lo), lim_hi), _mm_cmpord_ps(n2, n2));
+            if constexpr (Mode != SvfLP) {
+                p1l[i] = _mm_cvtss_f32(v1);
+                p1r[i] = _mm_cvtss_f32(_mm_shuffle_ps(v1, v1, 1));
+            }
+            if constexpr (Mode != SvfBP) {
+                p2l[i] = _mm_cvtss_f32(v2);
+                p2r[i] = _mm_cvtss_f32(_mm_shuffle_ps(v2, v2, 1));
+            }
+        }
+        state.ic1eq[0] = _mm_cvtss_f32(ic1);
+        state.ic1eq[1] = _mm_cvtss_f32(_mm_shuffle_ps(ic1, ic1, 1));
+        state.ic2eq[0] = _mm_cvtss_f32(ic2);
+        state.ic2eq[1] = _mm_cvtss_f32(_mm_shuffle_ps(ic2, ic2, 1));
+    }
+
+    // Pass 3: output mix.
+    {
+        const __m256 zero = _mm256_setzero_ps();
+        const __m256 one = _mm256_set1_ps(1.0f);
+        for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+            const __m256 xl = _mm256_load_ps(in_l + i);
+            const __m256 xr = _mm256_load_ps(in_r + i);
+            const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i) : zero;
+            const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : one;
+            __m256 pl, pr;
+            if constexpr (Mode == SvfLP) {
+                pl = _mm256_load_ps(p2l + i);
+                pr = _mm256_load_ps(p2r + i);
+            } else if constexpr (Mode == SvfBP) {
+                pl = _mm256_load_ps(p1l + i);
+                pr = _mm256_load_ps(p1r + i);
+            } else {
+                const __m256 k = _mm256_load_ps(kk + i);
+                pl = _mm256_sub_ps(_mm256_sub_ps(xl, _mm256_mul_ps(k, _mm256_load_ps(p1l + i))),
+                                   _mm256_load_ps(p2l + i));
+                pr = _mm256_sub_ps(_mm256_sub_ps(xr, _mm256_mul_ps(k, _mm256_load_ps(p1r + i))),
+                                   _mm256_load_ps(p2r + i));
+            }
+            _mm256_store_ps(out_l + i, _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(pl, wet)));
+            _mm256_store_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xr, dry), _mm256_mul_ps(pr, wet)));
+        }
+    }
+
+    // Leave the cached coefficients as the scalar path would after the last sample.
+    calc_svf(state, freq[BLOCK_SIZE - 1], q[BLOCK_SIZE - 1], sample_rate);
+}
+
+// Returns true when the AVX2 kernel handled the instruction.
+template <int Mode>
+[[gnu::always_inline]]
+inline bool svf_try_avx2(ExecutionContext& ctx, const Instruction& inst) {
+    if (!simd::has(simd::Isa::Avx2)) return false;
+    float* out_l = ctx.buffers->get(inst.out_buffer);
+    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
+    const float* input = ctx.buffers->get(inst.inputs
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


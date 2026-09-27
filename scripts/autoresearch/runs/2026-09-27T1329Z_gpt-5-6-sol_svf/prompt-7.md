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
        kernels::svf_stereo_dispatch_avx2<0>(out_l, out_r, input,
            stereo_in ? input_r : input, freq, q, dry_level, wet_level,
            ctx.sample_rate, state);
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
        kernels::svf_stereo_dispatch_avx2<1>(out_l, out_r, input,
            stereo_in ? input_r : input, freq, q, dry_level, wet_level,
            ctx.sample_rate, state);
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
        kernels::svf_stereo_dispatch_avx2<2>(out_l, out_r, input,
            stereo_in ? input_r : input, freq, q, dry_level, wet_level,
            ctx.sample_rate, state);
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
| `op_filter_svf_lp` | 2256.17 | 859.9 | 2.62x |
| `op_filter_svf_hp` | 2354.72 | 906.92 | 2.60x |
| `op_filter_svf_bp` | 2240.25 | 851.48 | 2.63x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..22de0ab 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -1,5 +1,7 @@
 #pragma once
 
+#include "../dsp/simd.hpp"
+
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
@@ -44,6 +46,187 @@ inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
 }
 
 // SVF Lowpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
+namespace kernels {
+#if CEDAR_SIMD_X86
+// simd: approx tan -- odd Taylor polynomial after reducing to [0, pi/4].
+// The SVF clamps its argument below pi/2.
+CEDAR_TARGET_AVX2 inline float svf_tan_avx2(float x) noexcept {
+    constexpr float half_pi = 1.57079632679489661923f;
+    constexpr float quarter_pi = 0.78539816339744830962f;
+    const bool reciprocal = x > quarter_pi;
+    const float r = reciprocal ? half_pi - x : x;
+    const float z = r * r;
+    float p = 443861162.0f / 1856156927625.0f;
+    p = p * z + 6404582.0f / 10854718875.0f;
+    p = p * z + 929569.0f / 638512875.0f;
+    p = p * z + 21844.0f / 6081075.0f;
+    p = p * z + 1382.0f / 155925.0f;
+    p = p * z + 62.0f / 2835.0f;
+    p = p * z + 17.0f / 315.0f;
+    p = p * z + 2.0f / 15.0f;
+    p = p * z + 1.0f / 3.0f;
+    const float tangent = r + r * z * p;
+    return reciprocal ? 1.0f / tangent : tangent;
+}
+
+CEDAR_TARGET_AVX2 inline void calc_svf_avx2(
+    SVFState& state, float freq, float q, float sample_rate) noexcept {
+    const float clamped_freq = std::min(freq, sample_rate * 0.45f);
+    const float g = svf_tan_avx2(PI * clamped_freq / sample_rate);
+    const float k = 1.0f / std::max(q, 0.01f);
+    state.a1 = 1.0f / (1.0f + g * (g + k));
+    state.a2 = g * state.a1;
+    state.a3 = g * state.a2;
+    state.k = k;
+}
+
+// simd: approx tan -- evaluate eight independent coefficient sets together.
+CEDAR_TARGET_AVX2 inline void calc_svf8_avx2(
+    const float* freq, const float* q, float sample_rate,
+    float* a1_out, float* a2_out, float* a3_out, float* k_out) noexcept {
+    const __m256 sample_rate_v = _mm256_set1_ps(sample_rate);
+    const __m256 max_freq = _mm256_mul_ps(
+        sample_rate_v, _mm256_set1_ps(0.45f));
+    // Put the source values second to match std::min/max for NaNs.
+    const __m256 clamped_freq = _mm256_min_ps(
+        max_freq, _mm256_load_ps(freq));
+    const __m256 x = _mm256_div_ps(
+        _mm256_mul_ps(_mm256_set1_ps(PI), clamped_freq), sample_rate_v);
+    const __m256 half_pi = _mm256_set1_ps(1.57079632679489661923f);
+    const __m256 quarter_pi = _mm256_set1_ps(0.78539816339744830962f);
+    const __m256 reciprocal = _mm256_cmp_ps(x, quarter_pi, _CMP_GT_OQ);
+    const __m256 r = _mm256_blendv_ps(x, _mm256_sub_ps(half_pi, x), reciprocal);
+    const __m256 z = _mm256_mul_ps(r, r);
+    __m256 p = _mm256_set1_ps(443861162.0f / 1856156927625.0f);
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(6404582.0f / 10854718875.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(929569.0f / 638512875.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(21844.0f / 6081075.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(1382.0f / 155925.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(62.0f / 2835.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(17.0f / 315.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(2.0f / 15.0f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, z), _mm256_set1_ps(1.0f / 3.0f));
+    const __m256 tangent = _mm256_add_ps(
+        r, _mm256_mul_ps(_mm256_mul_ps(r, z), p));
+    const __m256 g = _mm256_blendv_ps(
+        tangent, _mm256_div_ps(_mm256_set1_ps(1.0f), tangent), reciprocal);
+    const __m256 k = _mm256_div_ps(
+        _mm256_set1_ps(1.0f),
+        _mm256_max_ps(_mm256_set1_ps(0.01f), _mm256_load_ps(q)));
+    const __m256 a1 = _mm256_div_ps(
+        _mm256_set1_ps(1.0f),
+        _mm256_add_ps(_mm256_set1_ps(1.0f),
+            _mm256_mul_ps(g, _mm256_add_ps(g, k))));
+    const __m256 a2 = _mm256_mul_ps(g, a1);
+    const __m256 a3 = _mm256_mul_ps(g, a2);
+    _mm256_store_ps(a1_out, a1);
+    _mm256_store_ps(a2_out, a2);
+    _mm256_store_ps(a3_out, a3);
+    _mm256_store_ps(k_out, k);
+}
+
+// The filter's time-domain recurrence is serial, but its two channel recurrences
+// are independent.  Keep the coefficient calculation scalar and evaluate both
+// channels in one vector without changing the operation order within a lane.
+template <int Output, bool HasDry, bool HasWet>
+CEDAR_TARGET_AVX2 inline void svf_stereo_avx2(
+    float* out_l, float* out_r, const float* input_l, const float* input_r,
+    const float* freq, const float* q, const float* dry_level,
+    const float* wet_level, float sample_rate, SVFState& state) noexcept {
+    const __m128 denormal_dc = _mm_set1_ps(DENORMAL_DC);
+    const __m128 two = _mm_set1_ps(2.0f);
+    const __m128 clamp_lo = _mm_set1_ps(clamp_audio(-__builtin_inff()));
+    const __m128 clamp_hi = _mm_set1_ps(clamp_audio(__builtin_inff()));
+    __m128 old_ic1 = _mm_setr_ps(
+        state.ic1eq[0], state.ic1eq[1], 0.0f, 0.0f);
+    __m128 old_ic2 = _mm_setr_ps(
+        state.ic2eq[0], state.ic2eq[1], 0.0f, 0.0f);
+
+    alignas(32) float a1[8], a2[8], a3[8], k[8];
+    for (std::size_t base = 0; base < BLOCK_SIZE; base += 8) {
+        calc_svf8_avx2(freq + base, q + base, sample_rate, a1, a2, a3, k);
+        for (std::size_t lane = 0; lane < 8; ++lane) {
+        const std::size_t i = base + lane;
+        state.a1 = a1[lane];
+        state.a2 = a2[lane];
+        state.a3 = a3[lane];
+        state.k = k[lane];
+        const float dry = HasDry ? dry_level[i] : 0.0f;
+        const float wet = HasWet ? wet_level[i] : 1.0f;
+        const float xl = input_l[i];
+        const float xr = input_r[i];
+
+        const __m128 x = _mm_setr_ps(xl, xr, 0.0f, 0.0f);
+        const __m128 ic1 = _mm_add_ps(old_ic1, denormal_dc);
+        const __m128 ic2 = _mm_add_ps(old_ic2, denormal_dc);
+        const __m128 v3 = _mm_sub_ps(x, ic2);
+        const __m128 v1 = _mm_add_ps(
+            _mm_mul_ps(_mm_set1_ps(state.a1), ic1),
+            _mm_mul_ps(_mm_set1_ps(state.a2), v3));
+        const __m128 v2 = _mm_add_ps(
+            _mm_add_ps(ic2, _mm_mul_ps(_mm_set1_ps(state.a2), ic1)),
+            _mm_mul_ps(_mm_set1_ps(state.a3), v3));
+        const __m128 next_ic1 = _mm_sub_ps(_mm_mul_ps(two, v1), old_ic1);
+        const __m128 next_ic2 = _mm_sub_ps(_mm_mul_ps(two, v2), old_ic2);
+
+        // Put the value in the second operand so min/max preserve an in-range
+        // signed zero and propagate its NaN payload like clamp_audio does.
+        const __m128 clamped_ic1 = _mm_min_ps(
+            clamp_hi, _mm_max_ps(clamp_lo, next_ic1));
+        const __m128 clamped_ic2 = _mm_min_ps(
+            clamp_hi, _mm_max_ps(clamp_lo, next_ic2));
+        old_ic1 = clamped_ic1;
+        old_ic2 = clamped_ic2;
+
+        __m128 filtered;
+        if constexpr (Output == 0) {
+            filtered = v2;
+        } else if constexpr (Output == 1) {
+            filtered = _mm_sub_ps(
+                _mm_sub_ps(x, _mm_mul_ps(_mm_set1_ps(state.k), v1)), v2);
+        } else {
+            filtered = v1;
+        }
+        const __m128 mixed = _mm_add_ps(
+            _mm_mul_ps(x, _mm_set1_ps(dry)),
+            _mm_mul_ps(filtered, _mm_set1_ps(wet)));
+        out_l[i] = _mm_cvtss_f32(mixed);
+        out_r[i] = _mm_cvtss_f32(
+            _mm_shuffle_ps(mixed, mixed, _MM_SHUFFLE(1, 1, 1, 1)));
+        }
+    }
+
+    state.ic1eq[0] = _mm_cvtss_f32(old_ic1);
+    state.ic1eq[1] = _mm_cvtss_f32(_mm_shuffle_ps(
+        old_ic1, old_ic1, _MM_SHUFFLE(1, 1, 1, 1)));
+    state.ic2eq[0] = _mm_cvtss_f32(old_ic2);
+    state.ic2eq[1] = _mm_cvtss_f32(_mm_shuffle_ps(
+        old_ic2, old_ic2, _MM_SHUFFLE(1, 1, 1, 1)));
+}
+
+templat
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T1329Z_gpt-5-6-sol_svf/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | serial filter recurrence with independent stereo channels -> evaluate both channel recurrences in parallel SIMD lanes (check passed exact equality and showed 8–10% speedups) | filter_svf_lp 1.095x, filter_svf_hp 1.081x, filter_svf_bp 1.091x |
| 2 | ACCEPTED | scalar state clamping and output mixing inside the serial recurrence -> perform clamps, filter selection, and dry/wet mixing in two SIMD lanes (check passed exact equality and showed 32–33% speedups) | filter_svf_lp 1.322x, filter_svf_hp 1.332x, filter_svf_bp 1.319x |
| 3 | ACCEPTED | per-sample packing and memory writeback of the serial filter state -> keep both channel states in SIMD registers for the whole block and store once (check passed exact equality and showed 6–8% speedup | filter_svf_lp 1.07x, filter_svf_hp 1.061x, filter_svf_bp 1.075x |
| 4 | ACCEPTED | per-sample `tan` dominates coefficient generation -> replace it in the AVX2 path with a range-reduced degree-19 odd polynomial (check passed at -116.9 dBFS and showed 14–17% speedups) | filter_svf_lp 1.153x, filter_svf_hp 1.156x, filter_svf_bp 1.146x |
| 5 | ACCEPTED | serial scalar coefficient generation dominated by three per-sample divisions -> generate eight independent coefficient sets together with AVX2 before consuming them in recurrence order (check passed a | filter_svf_lp 1.472x, filter_svf_hp 1.467x, filter_svf_bp 1.493x |
| 6 | rejected: `equality` — op_filter_svf_hp: null test -92.9 dBFS > -100.0 dBFS; hidden randomised stimuli (seed 2109922690): first diff at sample  | serial dependency chain in the tangent polynomial -> evaluate the same coefficients with a parallel Estrin tree (check passed at -116.9 dBFS and showed 13–15% speedups) | — |

Full verifier output for the latest rejection (attempt 6, gate `equality`):
```
op_filter_svf_hp: null test -92.9 dBFS > -100.0 dBFS; hidden randomised stimuli (seed 2109922690): first diff at sample 42 (block 0): ref np.float32(-1.7545543e-18) vs candidate np.float32(-1.754554e-18)
```


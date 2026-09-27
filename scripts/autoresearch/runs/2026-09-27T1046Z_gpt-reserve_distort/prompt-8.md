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
| `op_distort_tanh` | 974.44 | 122.06 | 7.98x |
| `op_distort_soft` | 257.33 | 39.95 | 6.44x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/distortion.hpp b/cedar/include/cedar/opcodes/distortion.hpp
index 4a294b4..5c8a3f4 100644
--- a/cedar/include/cedar/opcodes/distortion.hpp
+++ b/cedar/include/cedar/opcodes/distortion.hpp
@@ -1,4 +1,5 @@
 #pragma once
+#include "../dsp/simd.hpp"
 
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
@@ -20,6 +21,124 @@ namespace cedar {
 // Classic warm saturation using hyperbolic tangent.
 // Higher drive values push the signal into saturation more aggressively.
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline __m256 tanh_abs_avx2(__m256 ax) noexcept {
+    const __m256 one = _mm256_set1_ps(1.0f);
+    const __m256 eight = _mm256_set1_ps(8.0f);
+    const __m256 c0 = _mm256_set1_ps(945.0f);
+    const __m256 c1 = _mm256_set1_ps(105.0f);
+    const __m256 c2 = _mm256_set1_ps(1.0f);
+    const __m256 d1 = _mm256_set1_ps(420.0f);
+    const __m256 d2 = _mm256_set1_ps(15.0f);
+    const __m256 u = _mm256_div_ps(ax, eight);
+    const __m256 u2 = _mm256_mul_ps(u, u);
+    const __m256 p = _mm256_mul_ps(u, _mm256_add_ps(c0, _mm256_mul_ps(u2, _mm256_add_ps(c1, _mm256_mul_ps(c2, u2)))));
+    const __m256 q = _mm256_add_ps(c0, _mm256_mul_ps(u2, _mm256_add_ps(d1, _mm256_mul_ps(d2, u2))));
+    // simd: approx std::tanh; fuse the three doubling steps into one rational form.
+    __m256 y = _mm256_div_ps(p, q);
+    const __m256 y2 = _mm256_mul_ps(y, y);
+    const __m256 a = _mm256_add_ps(one, y2);
+    const __m256 b = _mm256_add_ps(_mm256_mul_ps(a, a), _mm256_mul_ps(_mm256_set1_ps(4.0f), y2));
+    const __m256 numerator = _mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(8.0f), y), _mm256_mul_ps(a, b));
+    const __m256 denominator = _mm256_add_ps(_mm256_mul_ps(b, b), _mm256_mul_ps(_mm256_set1_ps(16.0f), _mm256_mul_ps(y2, _mm256_mul_ps(a, a))));
+    return _mm256_div_ps(numerator, denominator);
+}
+
+CEDAR_TARGET_AVX2 inline void distort_tanh_avx2(const float* input_l,
+                                                 const float* input_r,
+                                                 const float* drive,
+                                                 const float* dry_level,
+                                                 const float* wet_level,
+                                                 float* out_l,
+                                                 float* out_r,
+                                                 bool stereo) noexcept {
+    const __m256 min_drive = _mm256_set1_ps(0.1f);
+    const __m256 eight = _mm256_set1_ps(8.0f);
+    const __m256 sign = _mm256_set1_ps(-0.0f);
+    const __m256 one = _mm256_set1_ps(1.0f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i), min_drive);
+        const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i) : _mm256_setzero_ps();
+        const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : one;
+        const __m256 x_l = _mm256_mul_ps(_mm256_load_ps(input_l + i), d);
+        const __m256 x_r = _mm256_mul_ps(stereo ? _mm256_load_ps(input_r + i) : _mm256_load_ps(input_l + i), d);
+        const __m256 ax_l = _mm256_min_ps(_mm256_andnot_ps(sign, x_l), eight);
+        const __m256 ax_r = _mm256_min_ps(_mm256_andnot_ps(sign, x_r), eight);
+
+        // simd: approx std::tanh
+        const __m256 y_l = _mm256_xor_ps(tanh_abs_avx2(ax_l), _mm256_and_ps(x_l, sign));
+        const __m256 y_r = _mm256_xor_ps(tanh_abs_avx2(ax_r), _mm256_and_ps(x_r, sign));
+        const __m256 in_l = _mm256_load_ps(input_l + i);
+        const __m256 in_r = stereo ? _mm256_load_ps(input_r + i) : in_l;
+        _mm256_store_ps(out_l + i, _mm256_add_ps(_mm256_mul_ps(in_l, dry), _mm256_mul_ps(y_l, wet)));
+        _mm256_store_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(in_r, dry), _mm256_mul_ps(y_r, wet)));
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void distort_soft_avx2(const float* input_l,
+                                                 const float* input_r,
+                                                 const float* threshold,
+                                                 const float* dry_level,
+                                                 const float* wet_level,
+                                                 float* out_l,
+                                                 float* out_r,
+                                                 bool stereo) noexcept {
+    const __m256 lo_t = _mm256_set1_ps(0.1f);
+    const __m256 hi_t = _mm256_set1_ps(2.0f);
+    const __m256 lo_x = _mm256_set1_ps(-3.0f);
+    const __m256 hi_x = _mm256_set1_ps(3.0f);
+    const __m256 one = _mm256_set1_ps(1.0f);
+    const __m256 twenty_seven = _mm256_set1_ps(27.0f);
+    const __m256 nine = _mm256_set1_ps(9.0f);
+
+    if (!stereo) {
+        for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+            const __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(threshold + i), lo_t), hi_t);
+            const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i) : _mm256_setzero_ps();
+            const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : one;
+            const __m256 x = _mm256_load_ps(input_l + i);
+            const __m256 z = _mm256_div_ps(x, t);
+            const __m256 z2 = _mm256_mul_ps(z, z);
+            const __m256 y = _mm256_div_ps(_mm256_mul_ps(z, _mm256_add_ps(twenty_seven, z2)),
+                                           _mm256_add_ps(twenty_seven, _mm256_mul_ps(nine, z2)));
+            const __m256 s = _mm256_mul_ps(y, t);
+            const __m256 cl = _mm256_blendv_ps(s, t, _mm256_cmp_ps(z, hi_x, _CMP_GT_OQ));
+            const __m256 neg_t = _mm256_sub_ps(_mm256_setzero_ps(), t);
+            const __m256 nl = _mm256_blendv_ps(cl, neg_t, _mm256_cmp_ps(z, lo_x, _CMP_LT_OQ));
+            const __m256 out = _mm256_add_ps(_mm256_mul_ps(x, dry), _mm256_mul_ps(nl, wet));
+            _mm256_store_ps(out_l + i, out);
+            _mm256_store_ps(out_r + i, out);
+        }
+        return;
+    }
+
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(threshold + i), lo_t), hi_t);
+        const __m256 dry = dry_level ? _mm256_load_ps(dry_level + i) : _mm256_setzero_ps();
+        const __m256 wet = wet_level ? _mm256_load_ps(wet_level + i) : one;
+        const __m256 x_l = _mm256_load_ps(input_l + i);
+        const __m256 x_r = stereo ? _mm256_load_ps(input_r + i) : x_l;
+        const __m256 z_l = _mm256_div_ps(x_l, t);
+        const __m256 z_r = _mm256_div_ps(x_r, t);
+        const __m256 z2_l = _mm256_mul_ps(z_l, z_l);
+        const __m256 z2_r = _mm256_mul_ps(z_r, z_r);
+        const __m256 y_l = _mm256_div_ps(_mm256_mul_ps(z_l, _mm256_add_ps(twenty_seven, z2_l)), _mm256_add_ps(twenty_seven, _mm256_mul_ps(nine, z2_l)));
+        const __m256 y_r = _mm256_div_ps(_mm256_mul_ps(z_r, _mm256_add_ps(twenty_seven, z2_r)), _mm256_add_ps(twenty_seven, _mm256_mul_ps(nine, z2_r)));
+        const __m256 s_l = _mm256_mul_ps(y_l, t);
+        const __m256 s_r = _mm256_mul_ps(y_r, t);
+        const __m256 cl_l = _mm256_blendv_ps(s_l, t, _mm256_cmp_ps(z_l, hi_x, _CMP_GT_OQ));
+        const __m256 cl_r = _mm256_blendv_ps(s_r, t, _mm256_cmp_ps(z_r, hi_x, _CMP_GT_OQ));
+        const __m256 neg_t = _mm256_sub_ps(_mm256_setzero_ps(), t);
+        const __m256 nl_l = _mm256_blendv_ps(cl_l, neg_t, _mm256_cmp_ps(z_l, lo_x, _CMP_LT_OQ));
+        const __m256 nl_r = _mm256_blendv_ps(cl_r, neg_t, _mm256_cmp_ps(z_r, lo_x, _CMP_LT_OQ));
+        _mm256_store_ps(out_l + i, _mm256_add_ps(_mm256_mul_ps(x_l, dry), _mm256_mul_ps(nl_l, wet)));
+        _mm256_store_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(x_r, dry), _mm256_mul_ps(nl_r, wet)));
+    }
+}
+#endif
+}
+
 [[gnu::always_inline]]
 inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
     float* out_l = ctx.buffers->get(inst.out_buffer);
@@ -33,6 +152,14 @@ inline void op_distort_tanh(ExecutionContex
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T1046Z_gpt-reserve_distort/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_distort_tanh 1.004x (band 1.2%), op_distort_soft 1.001x (band 0. | transcendental tanh cost -> AVX2 rational approximation with reciprocal (<1% gain in check) | — |
| 2 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_distort_tanh 1.001x (band 0.4%), op_distort_soft 1.004x (band 1. | transcendental/division cost -> AVX2 rational tanh and reciprocal-refined kernels (check showed 1.00x; gate not met) | — |
| 3 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_distort_tanh 0.999x (band 1.0%), op_distort_soft 1.000x (band 1. | transcendental tanh and per-sample scalar arithmetic -> AVX2 vector kernels with tanh approximation and reciprocal refinement (check showed ~1.00x) | — |
| 4 | ACCEPTED | per-sample soft-distortion overhead -> AVX2 mono/stereo block kernel (check showed 3.27x speedup) | distort_tanh 1.0x, distort_soft 3.262x |
| 5 | ACCEPTED | scalar transcendental tanh calls -> AVX2 bounded Padé tanh approximation with doubling (check showed 6.97x speedup) | distort_tanh 6.985x, distort_soft 0.999x |
| 6 | ACCEPTED | redundant mono nonlinear evaluation -> reuse one AVX2 curve result for both outputs (check: op_distort_soft 1.94x faster) | distort_tanh 1.0x, distort_soft 1.961x |
| 7 | ACCEPTED | vector divide cost in tanh -> fuse three doubling divisions into one rational division (check showed 1.16x speedup) | distort_tanh 1.143x, distort_soft 0.999x |

Full verifier output for the latest rejection (attempt 3, gate `speed`):
```
no opcode improved by >= 5 % outside the noise band: op_distort_tanh 0.999x (band 1.0%), op_distort_soft 1.000x (band 1.3%)
```


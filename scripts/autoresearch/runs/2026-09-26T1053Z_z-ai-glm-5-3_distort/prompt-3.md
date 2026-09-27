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
        if (stereo_in) {
            kernels::distort_tanh_avx2<true>(input, input_r, drive, dry_level, wet_level, out_l, out_r);
        } else {
            kernels::distort_tanh_avx2<false>(input, input, drive, dry_level, wet_level, out_l, out_r);
        }
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

    auto soft = [](float x) {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    };

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        if (stereo_in) {
            kernels::distort_soft_avx2<true>(input, input_r, threshold, dry_level, wet_level, out_l, out_r);
        } else {
            kernels::distort_soft_avx2<false>(input, input, threshold, dry_level, wet_level, out_l, out_r);
        }
        return;
    }
#endif

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
| `op_distort_tanh` | 974.44 | 67.14 | 14.51x |
| `op_distort_soft` | 257.33 | 39.79 | 6.47x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/distortion.hpp b/cedar/include/cedar/opcodes/distortion.hpp
index 4a294b4..0039db0 100644
--- a/cedar/include/cedar/opcodes/distortion.hpp
+++ b/cedar/include/cedar/opcodes/distortion.hpp
@@ -3,6 +3,7 @@
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
+#include "../dsp/simd.hpp"
 #include "dsp_state.hpp"
 #include "dsp_utils.hpp"
 #include "drywet.hpp"
@@ -11,6 +12,177 @@
 
 namespace cedar {
 
+// ============================================================================
+// SIMD kernels (x86-64 AVX2) — docs/prd-simd-autoresearch.md §5.1
+//
+// The scalar bodies further down are untouched: they stay the oracle and the
+// fallback, and still run whenever simd::has(Isa::Avx2) is false. Buffers come
+// from the 32-byte aligned pool slabs, so the aligned load/store intrinsics
+// are safe.
+// ============================================================================
+namespace kernels {
+#if CEDAR_SIMD_X86
+
+static_assert(BLOCK_SIZE % 8 == 0, "AVX2 kernels need a multiple-of-8 block");
+
+// Per-8-lane dry/wet mix coefficient: control-buffer read, or the sentinel
+// fallback constant drywet::coeff() uses for an unconnected (0xFFFF) slot.
+template <bool FromBuf>
+CEDAR_TARGET_AVX2 inline __m256 coeff_avx2([[maybe_unused]] const float* buf,
+                                           [[maybe_unused]] std::size_t i, float fallback) noexcept {
+    if constexpr (FromBuf) {
+        return _mm256_load_ps(buf + i);
+    } else {
+        return _mm256_set1_ps(fallback);
+    }
+}
+
+// simd: approx std::tanh — tanh(x) = 1 - 2/(1 + e^(2x)), with e^(2x) split into
+// an exact power of two and a degree-8 polynomial for 2^s on s in [0,1).
+// Peak |error| is ~1e-7 (about -140 dBFS), inside the -100 dBFS budget this
+// opcode is allowed for its declared approximation; every other operation in
+// the opcode matches the scalar body bit for bit.
+CEDAR_TARGET_AVX2 inline __m256 tanh_avx2(__m256 x) noexcept {
+    const __m256 one = _mm256_set1_ps(1.0f);
+    // z = 2*log2(e)*x, clamped into the normal-exponent range. tanh() has
+    // saturated to +-1 long before that, so the clamp only touches samples
+    // whose result is exactly +-1 anyway (and keeps the range reduction well
+    // defined for |x| up to infinity). vminps/vmaxps return their second
+    // operand for NaN, so this ordering propagates NaN like std::tanh does.
+    __m256 z = _mm256_mul_ps(x, _mm256_set1_ps(2.885390081777926814f));
+    z = _mm256_min_ps(_mm256_set1_ps(126.0f), _mm256_max_ps(_mm256_set1_ps(-126.0f), z));
+    // z = k + s with s in [0,1)  ->  2^z = 2^k * 2^s (exact split)
+    const __m256i k = _mm256_cvttps_epi32(z);
+    const __m256 s = _mm256_sub_ps(z, _mm256_cvtepi32_ps(k));
+    // 2^s on [0,1): Taylor series around zero, truncation error <= 1.1e-7
+    // relative (coefficients (ln2)^n / n!).
+    __m256 p = _mm256_set1_ps(1.3215e-6f);
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), _mm256_set1_ps(1.5252734e-5f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), _mm256_set1_ps(1.5403531e-4f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), _mm256_set1_ps(1.3333559e-3f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), _mm256_set1_ps(9.6181291e-3f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), _mm256_set1_ps(5.5504109e-2f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), _mm256_set1_ps(2.4022651e-1f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), _mm256_set1_ps(6.9314718e-1f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, s), one);
+    const __m256 scale = _mm256_castsi256_ps(
+        _mm256_slli_epi32(_mm256_add_epi32(k, _mm256_set1_epi32(127)), 23));
+    const __m256 e = _mm256_mul_ps(p, scale);
+    return _mm256_sub_ps(one, _mm256_div_ps(_mm256_set1_ps(2.0f), _mm256_add_ps(one, e)));
+}
+
+// soft() from op_distort_soft over 8 lanes. Same operation order as the scalar
+// expression, so the |q| <= 3 lanes are bit-identical; the early-outs become
+// selects that return the same exact +-1 the branches return (the polynomial
+// is monotone and passes exactly through (3, 1) and (-3, -1)).
+CEDAR_TARGET_AVX2 inline __m256 soft_avx2(__m256 x, __m256 t) noexcept {
+    const __m256 q = _mm256_div_ps(x, t);  // x / t
+    const __m256 q2 = _mm256_mul_ps(q, q);
+    __m256 w = _mm256_div_ps(_mm256_mul_ps(q, _mm256_add_ps(_mm256_set1_ps(27.0f), q2)),
+                             _mm256_add_ps(_mm256_set1_ps(27.0f),
+                                           _mm256_mul_ps(_mm256_set1_ps(9.0f), q2)));
+    w = _mm256_blendv_ps(w, _mm256_set1_ps(1.0f),
+                         _mm256_cmp_ps(q, _mm256_set1_ps(3.0f), _CMP_GT_OQ));
+    w = _mm256_blendv_ps(w, _mm256_set1_ps(-1.0f),
+                         _mm256_cmp_ps(q, _mm256_set1_ps(-3.0f), _CMP_LT_OQ));
+    return _mm256_mul_ps(w, t);  // soft(x / t) * t
+}
+
+// DISTORT_TANH body over 8 lanes. `in_r` is only read when Stereo is set; a
+// mono input broadcasts the L result to R, exactly like the scalar body.
+template <bool Stereo, bool HasDry, bool HasWet>
+CEDAR_TARGET_AVX2 void distort_tanh_avx2(const float* in_l, [[maybe_unused]] const float* in_r,
+                                         const float* drive, [[maybe_unused]] const float* dry_level,
+                                         [[maybe_unused]] const float* wet_level, float* out_l,
+                                         float* out_r) noexcept {
+    const __m256 drive_floor = _mm256_set1_ps(0.1f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        // d = max(0.1f, drive): vmaxps(drive, 0.1f) so a NaN drive collapses
+        // to 0.1 exactly like the scalar std::max(0.1f, NaN).
+        const __m256 d = _mm256_max_ps(_mm256_load_ps(drive + i), drive_floor);
+        const __m256 dry = coeff_avx2<HasDry>(dry_level, i, 0.0f);
+        const __m256 wet = coeff_avx2<HasWet>(wet_level, i, 1.0f);
+        const __m256 x = _mm256_load_ps(in_l + i);
+        const __m256 sat = tanh_avx2(_mm256_mul_ps(x, d));
+        const __m256 res = _mm256_add_ps(_mm256_mul_ps(x, dry), _mm256_mul_ps(sat, wet));
+        _mm256_store_ps(out_l + i, res);
+        if constexpr (Stereo) {
+            const __m256 xr = _mm256_load_ps(in_r + i);
+            const __m256 sat_r = tanh_avx2(_mm256_mul_ps(xr, d));
+            _mm256_store_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xr, dry), _mm256_mul_ps(sat_r, wet)));
+        } else {
+            _mm256_store_ps(out_r + i, res);
+        }
+    }
+}
+
+// DISTORT_SOFT body over 8 lanes, same story as above.
+template <bool Stereo, bool HasDry, bool HasWet>
+CEDAR_TARGET_AVX2 void distort_soft_avx2(const float* in_l, [[maybe_unused]] const float* in_r,
+                                         const float* threshold, [[maybe_unused]] const float* dry_level,
+                                         [[maybe_unused]] const float* wet_level, float* out_l,
+                                         float* out_r) noexcept {
+    const __m256 lo = _mm256_set1_ps(0.1f);
+    const __m256 hi = _mm256_set1_ps(2.0f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        // t = clamp(threshold, 0.1, 2.0). Written as maxps(v, lo) then
+        // minps(..., hi) so the NaN/operand-order semantics match the scalar
+        // std::clamp(v, lo, hi) == min(hi, max(lo, v)) exactly (a NaN
+        // threshold collapses to lo, like std::max(0.1f, NaN) collapses to
+        // 0.1f in the tanh opcode's drive).
+        const __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(threshold + i), lo), hi);
+        const __m256 dry = coeff_avx2<HasDry>(dry_level, i, 0.0f);
+        const __m256 wet = coeff_avx2<HasWet>(wet_level, i, 1.0f);
+        const __m256 x = _mm256_load_ps(in_l + i);
+        const __m256 res = _mm256_add_ps(_mm256_mul_ps(x, dry), _mm256_mul_ps(soft_avx2(x, t), wet));
+        _mm256_store_ps(out_l + i, res);
+        if constexpr (Stereo) {
+            const __m256 xr = _mm256_load_ps(in_r + i);
+            const __m25
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1053Z_z-ai-glm-5-3_distort/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | rejected: `empty patch` — no files changed since the last accepted state | (no IDEA line) | — |
| 2 | ACCEPTED | scalar libm `tanhf` calls (tanh) and branchy per-sample scalar + dependent divides (soft) -> AVX2 kernels: exp2-based tanh with exact power-of-two range reduction, declared `// simd: approx`, and a bi | distort_tanh 14.738x, distort_soft 6.551x |

Full verifier output for the latest rejection (attempt 1, gate `empty patch`):
```
no files changed since the last accepted state
```


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
        distort_kernels::tanh_avx2(out_l, out_r, input, input_r, drive, dry_level, wet_level);
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
        distort_kernels::soft_avx2(out_l, out_r, input, input_r, threshold, dry_level, wet_level);
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
| `op_distort_tanh` | 974.44 | 51.82 | 18.80x |
| `op_distort_soft` | 257.33 | 49.29 | 5.22x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/distortion.hpp b/cedar/include/cedar/opcodes/distortion.hpp
index 4a294b4..6f24d70 100644
--- a/cedar/include/cedar/opcodes/distortion.hpp
+++ b/cedar/include/cedar/opcodes/distortion.hpp
@@ -6,11 +6,95 @@
 #include "dsp_state.hpp"
 #include "dsp_utils.hpp"
 #include "drywet.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
 
 namespace cedar {
 
+namespace distort_kernels {
+#if CEDAR_SIMD_X86
+
+// simd: approx std::tanh (rational minimax, |err| ~1e-7)
+CEDAR_TARGET_AVX2 inline __m256 tanh8(__m256 x) noexcept {
+    x = _mm256_max_ps(_mm256_min_ps(x, _mm256_set1_ps(7.99881172180175781f)),
+                      _mm256_set1_ps(-7.99881172180175781f));
+    const __m256 x2 = _mm256_mul_ps(x, x);
+    __m256 p = _mm256_set1_ps(-2.76076847742355e-16f);
+    p = _mm256_add_ps(_mm256_mul_ps(p, x2), _mm256_set1_ps(2.00018790482477e-13f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, x2), _mm256_set1_ps(-8.60467152213735e-11f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, x2), _mm256_set1_ps(5.12229709037114e-08f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, x2), _mm256_set1_ps(1.48572235717979e-05f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, x2), _mm256_set1_ps(6.37261928875436e-04f));
+    p = _mm256_add_ps(_mm256_mul_ps(p, x2), _mm256_set1_ps(4.89352455891786e-03f));
+    p = _mm256_mul_ps(p, x);
+    __m256 q = _mm256_set1_ps(1.19825839466702e-06f);
+    q = _mm256_add_ps(_mm256_mul_ps(q, x2), _mm256_set1_ps(1.18534705686654e-04f));
+    q = _mm256_add_ps(_mm256_mul_ps(q, x2), _mm256_set1_ps(2.26843463243900e-03f));
+    q = _mm256_add_ps(_mm256_mul_ps(q, x2), _mm256_set1_ps(4.89352518554385e-03f));
+    return _mm256_div_ps(p, q);
+}
+
+CEDAR_TARGET_AVX2 inline void tanh_avx2(float* out_l, float* out_r, const float* in_l,
+                                        const float* in_r, const float* drive,
+                                        const float* dry_level, const float* wet_level) noexcept {
+    const __m256 min_d = _mm256_set1_ps(0.1f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        const __m256 d = _mm256_max_ps(_mm256_loadu_ps(drive + i), min_d);
+        const __m256 dry = dry_level ? _mm256_loadu_ps(dry_level + i) : _mm256_setzero_ps();
+        const __m256 wet = wet_level ? _mm256_loadu_ps(wet_level + i) : _mm256_set1_ps(1.0f);
+        const __m256 xl = _mm256_loadu_ps(in_l + i);
+        const __m256 yl = tanh8(_mm256_mul_ps(xl, d));
+        _mm256_storeu_ps(out_l + i, _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(yl, wet)));
+        if (in_r) {
+            const __m256 xr = _mm256_loadu_ps(in_r + i);
+            const __m256 yr = tanh8(_mm256_mul_ps(xr, d));
+            _mm256_storeu_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xr, dry), _mm256_mul_ps(yr, wet)));
+        } else {
+            _mm256_storeu_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(yl, wet)));
+        }
+    }
+}
+
+CEDAR_TARGET_AVX2 inline __m256 soft8(__m256 x, __m256 t) noexcept {
+    const __m256 q = _mm256_div_ps(x, t);
+    const __m256 q2 = _mm256_mul_ps(q, q);
+    const __m256 num = _mm256_mul_ps(q, _mm256_add_ps(_mm256_set1_ps(27.0f), q2));
+    const __m256 den = _mm256_add_ps(_mm256_set1_ps(27.0f), _mm256_mul_ps(_mm256_set1_ps(9.0f), q2));
+    __m256 s = _mm256_div_ps(num, den);
+    s = _mm256_blendv_ps(s, _mm256_set1_ps(1.0f), _mm256_cmp_ps(q, _mm256_set1_ps(3.0f), _CMP_GT_OQ));
+    s = _mm256_blendv_ps(s, _mm256_set1_ps(-1.0f), _mm256_cmp_ps(q, _mm256_set1_ps(-3.0f), _CMP_LT_OQ));
+    return _mm256_mul_ps(s, t);
+}
+
+CEDAR_TARGET_AVX2 inline void soft_avx2(float* out_l, float* out_r, const float* in_l,
+                                        const float* in_r, const float* threshold,
+                                        const float* dry_level, const float* wet_level) noexcept {
+    const __m256 lo = _mm256_set1_ps(0.1f);
+    const __m256 hi = _mm256_set1_ps(2.0f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
+        // std::clamp(v, lo, hi): v<lo ? lo : (hi<v ? hi : v)
+        __m256 t = _mm256_loadu_ps(threshold + i);
+        t = _mm256_blendv_ps(t, hi, _mm256_cmp_ps(hi, t, _CMP_LT_OQ));
+        t = _mm256_blendv_ps(t, lo, _mm256_cmp_ps(t, lo, _CMP_LT_OQ));
+        const __m256 dry = dry_level ? _mm256_loadu_ps(dry_level + i) : _mm256_setzero_ps();
+        const __m256 wet = wet_level ? _mm256_loadu_ps(wet_level + i) : _mm256_set1_ps(1.0f);
+        const __m256 xl = _mm256_loadu_ps(in_l + i);
+        const __m256 yl = soft8(xl, t);
+        _mm256_storeu_ps(out_l + i, _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(yl, wet)));
+        if (in_r) {
+            const __m256 xr = _mm256_loadu_ps(in_r + i);
+            const __m256 yr = soft8(xr, t);
+            _mm256_storeu_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xr, dry), _mm256_mul_ps(yr, wet)));
+        } else {
+            _mm256_storeu_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(yl, wet)));
+        }
+    }
+}
+
+#endif
+}  // namespace distort_kernels
+
 // ============================================================================
 // DISTORT_TANH: Tanh Saturation
 // ============================================================================
@@ -33,6 +117,12 @@ inline void op_distort_tanh(ExecutionContext& ctx, const Instruction& inst) {
     const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
     const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        distort_kernels::tanh_avx2(out_l, out_r, input, input_r, drive, dry_level, wet_level);
+        return;
+    }
+#endif
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         float d = std::max(0.1f, drive[i]);
         float dry = drywet::coeff(dry_level, i, 0.0f);
@@ -66,6 +156,12 @@ inline void op_distort_soft(ExecutionContext& ctx, const Instruction& inst) {
     const float* dry_level = (inst.inputs[2] != 0xFFFF) ? ctx.buffers->get(inst.inputs[2]) : nullptr;
     const float* wet_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        distort_kernels::soft_avx2(out_l, out_r, input, input_r, threshold, dry_level, wet_level);
+        return;
+    }
+#endif
     auto soft = [](float x) {
         if (x > 3.0f) return 1.0f;
         if (x < -3.0f) return -1.0f;
```

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

## Method

1. Read the code you need.
2. **Before editing, state your hypothesis in one or two sentences:** what
   bounds this opcode's speed (e.g. a serial feedback recurrence, a
   transcendental call, memory bandwidth, branches, VM overhead) and the
   single change you will make to address it.
3. Make **one focused change**. Do not repeat an idea from the attempts table
   below unless you fix the reason it failed.
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-24T2050Z_sonnet_distort/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | scalar libm tanh call and scalar divides/branches per sample -> AVX2 8-wide kernels: rational-approx tanh (marked simd: approx), bit-exact soft clip via compare/blend, mono input computed once and sha | distort_tanh 18.77x, distort_soft 5.194x |


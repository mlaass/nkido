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
        kernels::filter_svf_avx2<0>(out_l, out_r, input, input_r, freq, q,
                                    dry_level, wet_level, stereo_in, state,
                                    ctx.sample_rate);
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
        kernels::filter_svf_avx2<1>(out_l, out_r, input, input_r, freq, q,
                                    dry_level, wet_level, stereo_in, state,
                                    ctx.sample_rate);
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
        kernels::filter_svf_avx2<2>(out_l, out_r, input, input_r, freq, q,
                                    dry_level, wet_level, stereo_in, state,
                                    ctx.sample_rate);
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
| `op_filter_svf_lp` | 2256.17 | 2122.77 | 1.06x |
| `op_filter_svf_hp` | 2354.72 | 2200.78 | 1.07x |
| `op_filter_svf_bp` | 2240.25 | 2117.99 | 1.06x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..26df3c1 100644
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
@@ -10,6 +11,55 @@
 
 namespace cedar {
 
+inline float clamp_audio(float val);
+inline void calc_svf(SVFState& state, float freq, float q, float sample_rate);
+constexpr float DENORMAL_DC = 1e-18f;
+
+#if CEDAR_SIMD_X86
+namespace kernels {
+
+template <int Mode>
+CEDAR_TARGET_AVX2 inline void filter_svf_avx2(
+    float* out_l, float* out_r, const float* input, const float* input_r,
+    const float* freq, const float* q, const float* dry_level,
+    const float* wet_level, bool stereo_in, SVFState& state,
+    float sample_rate) noexcept {
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        calc_svf(state, freq[i], q[i], sample_rate);
+        const float dry = drywet::coeff(dry_level, i, 0.0f);
+        const float wet = drywet::coeff(wet_level, i, 1.0f);
+
+        const float xl = input[i];
+        const float v3l = xl - (state.ic2eq[0] + DENORMAL_DC);
+        const float v1l = state.a1 * (state.ic1eq[0] + DENORMAL_DC) + state.a2 * v3l;
+        const float v2l = (state.ic2eq[0] + DENORMAL_DC) + state.a2 * (state.ic1eq[0] + DENORMAL_DC) + state.a3 * v3l;
+        state.ic1eq[0] = clamp_audio(2.0f * v1l - state.ic1eq[0]);
+        state.ic2eq[0] = clamp_audio(2.0f * v2l - state.ic2eq[0]);
+        if constexpr (Mode == 0)
+            out_l[i] = drywet::mix(xl, v2l, dry, wet);
+        else if constexpr (Mode == 1)
+            out_l[i] = drywet::mix(xl, xl - state.k * v1l - v2l, dry, wet);
+        else
+            out_l[i] = drywet::mix(xl, v1l, dry, wet);
+
+        const float xr = stereo_in ? input_r[i] : xl;
+        const float v3r = xr - (state.ic2eq[1] + DENORMAL_DC);
+        const float v1r = state.a1 * (state.ic1eq[1] + DENORMAL_DC) + state.a2 * v3r;
+        const float v2r = (state.ic2eq[1] + DENORMAL_DC) + state.a2 * (state.ic1eq[1] + DENORMAL_DC) + state.a3 * v3r;
+        state.ic1eq[1] = clamp_audio(2.0f * v1r - state.ic1eq[1]);
+        state.ic2eq[1] = clamp_audio(2.0f * v2r - state.ic2eq[1]);
+        if constexpr (Mode == 0)
+            out_r[i] = drywet::mix(xr, v2r, dry, wet);
+        else if constexpr (Mode == 1)
+            out_r[i] = drywet::mix(xr, xr - state.k * v1r - v2r, dry, wet);
+        else
+            out_r[i] = drywet::mix(xr, v1r, dry, wet);
+    }
+}
+
+}  // namespace kernels
+#endif
+
 // Clamp filter state/output to prevent blowup
 // Audio signals should never exceed ±10 in normal operation
 [[gnu::always_inline]]
@@ -20,9 +70,6 @@ inline float clamp_audio(float val) {
     return val;
 }
 
-// Tiny DC offset to prevent denormal numbers (inaudible)
-constexpr float DENORMAL_DC = 1e-18f;
-
 // SVF (State Variable Filter) coefficient calculation
 inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
     freq = std::max(0.0f, freq);
@@ -59,6 +106,15 @@ inline void op_filter_svf_lp(ExecutionContext& ctx, const Instruction& inst) {
     const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
     auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::filter_svf_avx2<0>(out_l, out_r, input, input_r, freq, q,
+                                    dry_level, wet_level, stereo_in, state,
+                                    ctx.sample_rate);
+        return;
+    }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         calc_svf(state, freq[i], q[i], ctx.sample_rate);
         float dry = drywet::coeff(dry_level, i, 0.0f);
@@ -92,6 +148,15 @@ inline void op_filter_svf_hp(ExecutionContext& ctx, const Instruction& inst) {
     const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
     auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::filter_svf_avx2<1>(out_l, out_r, input, input_r, freq, q,
+                                    dry_level, wet_level, stereo_in, state,
+                                    ctx.sample_rate);
+        return;
+    }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         calc_svf(state, freq[i], q[i], ctx.sample_rate);
         float dry = drywet::coeff(dry_level, i, 0.0f);
@@ -127,6 +192,15 @@ inline void op_filter_svf_bp(ExecutionContext& ctx, const Instruction& inst) {
     const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
     auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);
 
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::filter_svf_avx2<2>(out_l, out_r, input, input_r, freq, q,
+                                    dry_level, wet_level, stereo_in, state,
+                                    ctx.sample_rate);
+        return;
+    }
+#endif
+
     for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
         calc_svf(state, freq[i], q[i], ctx.sample_rate);
         float dry = drywet::coeff(dry_level, i, 0.0f);
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T1226Z_gpt-reserve_svf/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_filter_svf_lp 1.000x (band 0.2%), op_filter_svf_hp 0.999x (band  | transcendental coefficient calculation -> AVX2-batched tangent approximation (fixed equality passed; benchmark remained ~1.00x) | — |
| 2 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_filter_svf_lp 1.000x (band 0.1%), op_filter_svf_hp 1.000x (band  | serial feedback and coefficient cost -> packed AVX2 recurrence plus approximate tan (check showed no ≥5% speedup) | — |
| 3 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_filter_svf_lp 1.000x (band 0.6%), op_filter_svf_hp 1.000x (band  | serial feedback and helper overhead -> AVX2 packed recurrence plus batched tan and direct defaults (check passed exactness; no qualifying speedup) | — |
| 4 | ACCEPTED | serial stereo recurrence and channel-loop overhead -> AVX2-targeted branch-free stereo kernel (<what the check showed>) | filter_svf_lp 1.066x, filter_svf_hp 1.069x, filter_svf_bp 1.063x |
| 5 | rejected: `empty patch` — no files changed since the last accepted state | stereo recurrence arithmetic -> register-packed SIMD (check showed 1.00x, rejected) | — |
| 6 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_filter_svf_lp 1.000x (band 0.1%), op_filter_svf_hp 1.000x (band  | per-sample default-level branches -> hoist dry/wet null checks outside the loop (check showed ~1.00x, no qualifying speedup) | — |
| 7 | rejected: `equality` — op_filter_svf_hp: null test -83.5 dBFS > -100.0 dBFS; hidden randomised stimuli (seed 1024973547): first diff at sample  | transcendental coefficient cost -> range-reduced AVX2 tan approximation in the SIMD kernel (check showed 1.20–1.23x speedup, -116.9 dBFS error) | — |

Full verifier output for the latest rejection (attempt 7, gate `equality`):
```
op_filter_svf_hp: null test -83.5 dBFS > -100.0 dBFS; hidden randomised stimuli (seed 1024973547): first diff at sample 0 (block 0): ref np.float32(0.46852988) vs candidate np.float32(0.46852994)
```

**The last 3 attempts were all rejected.** Try a structurally different approach from everything in the table above (different data layout, different loop structure, a different opcode in the list) rather than a variation of them.


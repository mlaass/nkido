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
    if (simd::has(simd::Isa::Avx2) && kernels::svf_channels_avx2<0>(ctx, inst)) return;
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
    if (simd::has(simd::Isa::Avx2) && kernels::svf_channels_avx2<1>(ctx, inst)) return;
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
    if (simd::has(simd::Isa::Avx2) && kernels::svf_channels_avx2<2>(ctx, inst)) return;
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
| `op_filter_svf_lp` | 2256.17 | 1563.34 | 1.44x |
| `op_filter_svf_hp` | 2354.72 | 1617.75 | 1.46x |
| `op_filter_svf_bp` | 2240.25 | 1563.81 | 1.43x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/filters.hpp b/cedar/include/cedar/opcodes/filters.hpp
index c8d3d39..1fa378c 100644
--- a/cedar/include/cedar/opcodes/filters.hpp
+++ b/cedar/include/cedar/opcodes/filters.hpp
@@ -1,5 +1,7 @@
 #pragma once
 
+#include "../dsp/simd.hpp"
+
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
@@ -44,8 +46,115 @@ inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
 }
 
 // SVF Lowpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
+namespace kernels {
+#if CEDAR_SIMD_X86
+// The recurrence is serial in time, but its two channels are independent.
+// Keep the original helpers and operation ordering, including separate mul/add.
+template <int Mode, bool SharedChannels = false>
+CEDAR_TARGET_AVX2 inline bool svf_channels_avx2(
+    ExecutionContext& ctx, const Instruction& inst) noexcept {
+    float* out_l = ctx.buffers->get(inst.out_buffer);
+    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
+    const float* input = ctx.buffers->get(inst.inputs[0]);
+    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
+    const float* input_r = stereo_in
+        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
+        : input;
+    // The scalar loop reads the right channel after writing the left output.
+    // Preserve that ordering for an aliased right-channel source.
+    if (out_l == input_r) return false;
+    const float* freq = ctx.buffers->get(inst.inputs[1]);
+    const float* q = ctx.buffers->get(inst.inputs[2]);
+    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
+    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
+    auto& state = ctx.states->get_or_create<SVFState>(inst.state_id);
+    // Identical mono channels stay identical throughout the recurrence. Compare
+    // bits (including signed zero) before sharing the scalar clamp/mix work.
+    if constexpr (!SharedChannels) {
+        if (input_r == input) {
+            const __m128 histories = _mm_setr_ps(state.ic1eq[0], state.ic1eq[1],
+                                                state.ic2eq[0], state.ic2eq[1]);
+            const __m128 swapped = _mm_shuffle_ps(histories, histories, 0xB1);
+            if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_castps_si128(histories),
+                                                 _mm_castps_si128(swapped))) == 0xFFFF)
+                return svf_channels_avx2<Mode, true>(ctx, inst);
+        }
+    }
+    // A shared mono recurrence has only one live channel. Avoid constructing
+    // vectors and extracting their lanes around the scalar clamp/mix helpers.
+    if constexpr (SharedChannels) {
+        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+            calc_svf(state, freq[i], q[i], ctx.sample_rate);
+            const float dry = drywet::coeff(dry_level, i, 0.0f);
+            const float wet = drywet::coeff(wet_level, i, 1.0f);
+            const float x = input[i];
+            const float v3 = x - (state.ic2eq[0] + DENORMAL_DC);
+            const float v1 = state.a1 * (state.ic1eq[0] + DENORMAL_DC)
+                           + state.a2 * v3;
+            const float v2 = (state.ic2eq[0] + DENORMAL_DC)
+                           + state.a2 * (state.ic1eq[0] + DENORMAL_DC)
+                           + state.a3 * v3;
+            state.ic1eq[0] = clamp_audio(2.0f * v1 - state.ic1eq[0]);
+            state.ic2eq[0] = clamp_audio(2.0f * v2 - state.ic2eq[0]);
+            state.ic1eq[1] = state.ic1eq[0];
+            state.ic2eq[1] = state.ic2eq[0];
+            float result;
+            if constexpr (Mode == 0) result = v2;
+            else if constexpr (Mode == 1) result = x - state.k * v1 - v2;
+            else result = v1;
+            out_l[i] = drywet::mix(x, result, dry, wet);
+            out_r[i] = out_l[i];
+        }
+        return true;
+    }
+    const __m128 dc = _mm_set1_ps(DENORMAL_DC);
+    const __m128 two = _mm_set1_ps(2.0f);
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        calc_svf(state, freq[i], q[i], ctx.sample_rate);
+        const float dry = drywet::coeff(dry_level, i, 0.0f);
+        const float wet = drywet::coeff(wet_level, i, 1.0f);
+        const __m128 x = _mm_setr_ps(input[i], input_r[i], 0.0f, 0.0f);
+        const __m128 s1 = _mm_setr_ps(state.ic1eq[0], state.ic1eq[1], 0.0f, 0.0f);
+        const __m128 s2 = _mm_setr_ps(state.ic2eq[0], state.ic2eq[1], 0.0f, 0.0f);
+        const __m128 d1 = _mm_add_ps(s1, dc);
+        const __m128 d2 = _mm_add_ps(s2, dc);
+        const __m128 v3 = _mm_sub_ps(x, d2);
+        const __m128 a2 = _mm_set1_ps(state.a2);
+        const __m128 v1 = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(state.a1), d1),
+                                     _mm_mul_ps(a2, v3));
+        const __m128 v2 = _mm_add_ps(_mm_add_ps(d2, _mm_mul_ps(a2, d1)),
+                                     _mm_mul_ps(_mm_set1_ps(state.a3), v3));
+        const __m128 n1 = _mm_sub_ps(_mm_mul_ps(two, v1), s1);
+        const __m128 n2 = _mm_sub_ps(_mm_mul_ps(two, v2), s2);
+        state.ic1eq[0] = clamp_audio(_mm_cvtss_f32(n1));
+        state.ic2eq[0] = clamp_audio(_mm_cvtss_f32(n2));
+        if constexpr (SharedChannels) {
+            state.ic1eq[1] = state.ic1eq[0];
+            state.ic2eq[1] = state.ic2eq[0];
+        } else {
+            state.ic1eq[1] = clamp_audio(_mm_cvtss_f32(_mm_shuffle_ps(n1, n1, 0x55)));
+            state.ic2eq[1] = clamp_audio(_mm_cvtss_f32(_mm_shuffle_ps(n2, n2, 0x55)));
+        }
+        __m128 result;
+        if constexpr (Mode == 0) result = v2;
+        else if constexpr (Mode == 1)
+            result = _mm_sub_ps(_mm_sub_ps(x, _mm_mul_ps(_mm_set1_ps(state.k), v1)), v2);
+        else result = v1;
+        out_l[i] = drywet::mix(_mm_cvtss_f32(x), _mm_cvtss_f32(result), dry, wet);
+        if constexpr (SharedChannels) out_r[i] = out_l[i];
+        else out_r[i] = drywet::mix(_mm_cvtss_f32(_mm_shuffle_ps(x, x, 0x55)),
+                _mm_cvtss_f32(_mm_shuffle_ps(result, result, 0x55)), dry, wet);
+    }
+    return true;
+}
+#endif
+} // namespace kernels
+
 [[gnu::always_inline]]
 inline void op_filter_svf_lp(ExecutionContext& ctx, const Instruction& inst) {
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2) && kernels::svf_channels_avx2<0>(ctx, inst)) return;
+#endif
     float* out_l = ctx.buffers->get(inst.out_buffer);
     float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
     const float* input = ctx.buffers->get(inst.inputs[0]);
@@ -79,6 +188,9 @@ inline void op_filter_svf_lp(ExecutionContext& ctx, const Instruction& inst) {
 // SVF Highpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
 [[gnu::always_inline]]
 inline void op_filter_svf_hp(ExecutionContext& ctx, const Instruction& inst) {
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2) && kernels::svf_channels_avx2<1>(ctx, inst)) return;
+#endif
     float* out_l = ctx.buffers->get(inst.out_buffer);
     float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
     const float* input = ctx.buffers->get(inst.inputs[0]);
@@ -114,6 +226,9 @@ inline void op_filter_svf_hp(ExecutionContext& ctx, const Instruction& inst) {
 // SVF Bandpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
 [[gnu::always_inline]]
 inline void op_filter_svf_bp(ExecutionContext& ctx, const Instruction& inst) {
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2) && kernels::svf_channels_avx2<2>(ctx, inst)) return;
+#endif
     float* out_l = ctx.buffers->get(inst.out_buffer);
     float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
     const float* input = ctx.buffers->get(inst.inputs[0]);
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T2043Z_gpt-6-astra_svf/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | rejected: `empty patch` — no files changed since the last accepted state | bottleneck unconfirmed -> no changes made (check not run; file inspection blocked) | — |
| 2 | rejected: `empty patch` — no files changed since the last accepted state | bottleneck unconfirmed -> no change made pending permission to read files (check not run) | — |
| 3 | ACCEPTED | serial feedback with independent channels -> SIMD channel arithmetic retaining exact helpers (check passed exact equality; all three opcodes 1.06–1.07× faster) | filter_svf_lp 1.064x, filter_svf_hp 1.075x, filter_svf_bp 1.058x |
| 4 | ACCEPTED | duplicated channel clamp/mix work -> reuse results for mono input with bit-identical states (check passed exact equality; all three opcodes 1.28× faster) | filter_svf_lp 1.284x, filter_svf_hp 1.276x, filter_svf_bp 1.283x |
| 5 | ACCEPTED | SIMD packing overhead in the shared mono recurrence -> compute one scalar recurrence with exact helpers (check passed exact equality; LP, HP, BP each 1.06× faster) | filter_svf_lp 1.058x, filter_svf_hp 1.06x, filter_svf_bp 1.059x |

Full verifier output for the latest rejection (attempt 2, gate `empty patch`):
```
no files changed since the last accepted state
```


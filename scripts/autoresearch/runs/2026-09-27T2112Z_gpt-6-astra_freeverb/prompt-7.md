You are optimising Cedar DSP opcodes with SIMD. Cedar is the real-time audio
engine of nkido: a bytecode VM that processes blocks of 128 float samples at
48 kHz, zero allocations on the audio path. The repository is your current
working directory.

## Task

Make these opcodes faster on **x86-64 AVX2** (the gate machine):

- `op_reverb_freeverb`

They live in `cedar/include/cedar/opcodes/reverbs.hpp`. Current source of the opcode bodies:

```cpp
[[gnu::always_inline]]
inline void op_reverb_freeverb(ExecutionContext& ctx, const Instruction& inst) {
    // Stereo-native: writes out_buffer (L) and out_buffer+1 (R) in one call.
    // When STEREO_INPUT is set in inst.flags, the primary signal input is a
    // stereo pair (inputs[0] = L, inputs[0]+1 = R). When unset, the mono input
    // is auto-broadcast to both internal lanes. Each lane has its own full
    // 8-comb + 4-allpass network with Schroeder +23-sample length offsets on
    // the R lane for L/R decorrelation. See prd-stereo-native-opcodes.md for
    // the flag truth table.
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* room_size = ctx.buffers->get(inst.inputs[1]);
    const float* damping = ctx.buffers->get(inst.inputs[2]);
    const float* room_scale_in = ctx.buffers->get(inst.inputs[3]);
    const float* room_offset_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FreeverbState>(inst.state_id);

    // Ensure buffers are allocated from arena
    state.ensure_buffers(ctx.arena);
    if (!state.buffers_ready()) {  // arena exhausted — degrade to dry passthrough
        CEDAR_LOG_ONCE("[CEDAR] audio arena exhausted: reverb(freeverb) → dry passthrough\n");
        drywet::passthrough_stereo(input, input_r, stereo_in, out_l, out_r, BLOCK_SIZE);
        return;
    }

    // ExtendedParams<2>: ext[0] = dry (default 1.0), ext[1] = wet (default 0.5)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 1.0f;
    const float* wet_buf = nullptr; float wet_const = 0.5f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

    #if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::freeverb_avx2(state, input, input_r, stereo_in, room_size,
            damping, room_scale_in, room_offset_in, dry_buf, dry_const,
            wet_buf, wet_const, out_l, out_r);
        return;
    }
    #endif

    constexpr float ALLPASS_GAIN = 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x_l = input[i];
        float x_r = stereo_in ? input_r[i] : x_l;
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);
        float room = std::clamp(room_size[i], 0.0f, 1.0f);
        float damp = std::clamp(damping[i], 0.0f, 1.0f);

        // Runtime tunable parameters (use defaults if zero/negative)
        float room_scale = room_scale_in[i] > 0.0f ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
        float room_offset = room_offset_in[i] > 0.0f ? room_offset_in[i] : FREEVERB_ROOM_OFFSET_DEFAULT;

        // Feedback coefficient from room size
        float feedback = room * room_scale + room_offset;

        // Process L and R lanes with per-channel state and Schroeder-offset
        // delay-line sizes. The two lanes are otherwise structurally identical.
        float y_lr[2] = {0.0f, 0.0f};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? x_l : x_r;

            // Sum output from all 8 comb filters in parallel
            float comb_sum = 0.0f;
            for (std::size_t c = 0; c < FreeverbState::NUM_COMBS; ++c) {
                float* buffer = state.comb_buffers[ch][c];
                std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];

                // Read from delay line
                float delayed = buffer[state.comb_pos[ch][c]];

                // Lowpass filter in feedback path (damping)
                state.comb_filter_state[ch][c] =
                    delayed * (1.0f - damp) + state.comb_filter_state[ch][c] * damp;

                // DC blocker in feedback path to prevent low-frequency buildup
                float dc_in = state.comb_filter_state[ch][c];
                float dc_out = dc_in - state.dc_x1[ch][c] +
                               DC_BLOCKER_R * state.dc_y1[ch][c];
                state.dc_x1[ch][c] = dc_in;
                state.dc_y1[ch][c] = dc_out;

                // Write with feedback
                buffer[state.comb_pos[ch][c]] = x + feedback * dc_out;
                state.comb_pos[ch][c] = (state.comb_pos[ch][c] + 1) % size;

                comb_sum += delayed;
            }

            // Series allpass filters for diffusion
            float y = comb_sum;
            for (std::size_t a = 0; a < FreeverbState::NUM_ALLPASSES; ++a) {
                float* buffer = state.allpass_buffers[ch][a];
                std::size_t size = FreeverbState::ALLPASS_SIZES_LR[ch][a];

                float delayed = buffer[state.allpass_pos[ch][a]];
                float output = delayed - ALLPASS_GAIN * y;
                buffer[state.allpass_pos[ch][a]] = y + ALLPASS_GAIN * output;
                state.allpass_pos[ch][a] = (state.allpass_pos[ch][a] + 1) % size;

                y = output;
            }
            y_lr[ch] = y;
        }

        out_l[i] = drywet::mix(x_l, y_lr[0], dry, wet);
        out_r[i] = drywet::mix(x_r, y_lr[1], dry, wet);
    }
}
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_reverb_freeverb` | 6784.81 | 1521.71 | 4.46x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/reverbs.hpp b/cedar/include/cedar/opcodes/reverbs.hpp
index 0d74114..5f18514 100644
--- a/cedar/include/cedar/opcodes/reverbs.hpp
+++ b/cedar/include/cedar/opcodes/reverbs.hpp
@@ -1,5 +1,7 @@
 #pragma once
 
+#include "../dsp/simd.hpp"
+
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
@@ -33,6 +35,202 @@ constexpr float FREEVERB_ROOM_OFFSET_DEFAULT = 0.7f;
 // Classic algorithm: 8 parallel lowpass-feedback comb filters summed,
 // then through 4 series allpass filters. Creates lush, dense reverb.
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+// Exchange comb and time axes without doing any floating-point arithmetic.
+CEDAR_TARGET_AVX2 inline void freeverb_transpose8(__m256* v) noexcept {
+    const __m256 t0 = _mm256_unpacklo_ps(v[0], v[1]);
+    const __m256 t1 = _mm256_unpackhi_ps(v[0], v[1]);
+    const __m256 t2 = _mm256_unpacklo_ps(v[2], v[3]);
+    const __m256 t3 = _mm256_unpackhi_ps(v[2], v[3]);
+    const __m256 t4 = _mm256_unpacklo_ps(v[4], v[5]);
+    const __m256 t5 = _mm256_unpackhi_ps(v[4], v[5]);
+    const __m256 t6 = _mm256_unpacklo_ps(v[6], v[7]);
+    const __m256 t7 = _mm256_unpackhi_ps(v[6], v[7]);
+    const __m256 s0 = _mm256_shuffle_ps(t0, t2, 0x44);
+    const __m256 s1 = _mm256_shuffle_ps(t0, t2, 0xee);
+    const __m256 s2 = _mm256_shuffle_ps(t1, t3, 0x44);
+    const __m256 s3 = _mm256_shuffle_ps(t1, t3, 0xee);
+    const __m256 s4 = _mm256_shuffle_ps(t4, t6, 0x44);
+    const __m256 s5 = _mm256_shuffle_ps(t4, t6, 0xee);
+    const __m256 s6 = _mm256_shuffle_ps(t5, t7, 0x44);
+    const __m256 s7 = _mm256_shuffle_ps(t5, t7, 0xee);
+    v[0] = _mm256_permute2f128_ps(s0, s4, 0x20);
+    v[1] = _mm256_permute2f128_ps(s1, s5, 0x20);
+    v[2] = _mm256_permute2f128_ps(s2, s6, 0x20);
+    v[3] = _mm256_permute2f128_ps(s3, s7, 0x20);
+    v[4] = _mm256_permute2f128_ps(s0, s4, 0x31);
+    v[5] = _mm256_permute2f128_ps(s1, s5, 0x31);
+    v[6] = _mm256_permute2f128_ps(s2, s6, 0x31);
+    v[7] = _mm256_permute2f128_ps(s3, s7, 0x31);
+}
+
+// SIMD lanes are independent combs, not successive samples: the feedback
+// recurrence and the left-to-right comb sum retain their scalar ordering.
+CEDAR_TARGET_AVX2 inline void freeverb_avx2(
+    FreeverbState& state, const float* input, const float* input_r,
+    bool stereo_in, const float* room_size, const float* damping,
+    const float* room_scale_in, const float* room_offset_in,
+    const float* dry_buf, float dry_const, const float* wet_buf,
+    float wet_const, float* out_l, float* out_r) noexcept {
+    __m256 filter[2], dc_x[2], dc_y[2];
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        filter[ch] = _mm256_loadu_ps(&state.comb_filter_state[ch][0]);
+        dc_x[ch] = _mm256_loadu_ps(&state.dc_x1[ch][0]);
+        dc_y[ch] = _mm256_loadu_ps(&state.dc_y1[ch][0]);
+    }
+    const __m256 dc_r = _mm256_set1_ps(DC_BLOCKER_R);
+    // Within a span no delay line wraps. Keep positions fixed until its end,
+    // avoiding per-sample position stores and wrap checks for all 24 lines.
+    for (std::size_t base = 0; base < BLOCK_SIZE;) {
+        std::size_t count = BLOCK_SIZE - base;
+        float* comb[2][FreeverbState::NUM_COMBS];
+        float* allpass[2][FreeverbState::NUM_ALLPASSES];
+        for (std::size_t ch = 0; ch < 2; ++ch) {
+            for (std::size_t c = 0; c < FreeverbState::NUM_COMBS; ++c) {
+                const auto pos = state.comb_pos[ch][c];
+                count = std::min(count, FreeverbState::COMB_SIZES_LR[ch][c] - pos);
+                comb[ch][c] = state.comb_buffers[ch][c] + pos;
+            }
+            for (std::size_t a = 0; a < FreeverbState::NUM_ALLPASSES; ++a) {
+                const auto pos = state.allpass_pos[ch][a];
+                count = std::min(count, FreeverbState::ALLPASS_SIZES_LR[ch][a] - pos);
+                allpass[ch][a] = state.allpass_buffers[ch][a] + pos;
+            }
+        }
+        std::size_t j = 0;
+        for (; j + 8 <= count; j += 8) {
+            const std::size_t i = base + j;
+            float feedbacks[8], damps[8], undampeds[8];
+            float x[2][8], dry[8], wet[8];
+            for (std::size_t t = 0; t < 8; ++t) {
+                x[0][t] = input[i + t];
+                x[1][t] = stereo_in ? input_r[i + t] : x[0][t];
+                dry[t] = drywet::coeff(dry_buf, i + t, dry_const);
+                wet[t] = drywet::coeff(wet_buf, i + t, wet_const);
+                const float room = std::clamp(room_size[i + t], 0.0f, 1.0f);
+                damps[t] = std::clamp(damping[i + t], 0.0f, 1.0f);
+                undampeds[t] = 1.0f - damps[t];
+                const float scale = room_scale_in[i + t] > 0.0f
+                    ? room_scale_in[i + t] : FREEVERB_ROOM_SCALE_DEFAULT;
+                const float offset = room_offset_in[i + t] > 0.0f
+                    ? room_offset_in[i + t] : FREEVERB_ROOM_OFFSET_DEFAULT;
+                feedbacks[t] = room * scale + offset;
+            }
+            float y_lr[2][8];
+            for (std::size_t ch = 0; ch < 2; ++ch) {
+                __m256 tile[8];
+                __m256 y = _mm256_setzero_ps();
+                for (std::size_t c = 0; c < 8; ++c) {
+                    tile[c] = _mm256_loadu_ps(comb[ch][c] + j);
+                    // Each SIMD lane sums combs in the scalar order.
+                    y = _mm256_add_ps(y, tile[c]);
+                }
+                freeverb_transpose8(tile);
+                for (std::size_t t = 0; t < 8; ++t) {
+                    filter[ch] = _mm256_add_ps(
+                        _mm256_mul_ps(tile[t], _mm256_set1_ps(undampeds[t])),
+                        _mm256_mul_ps(filter[ch], _mm256_set1_ps(damps[t])));
+                    const __m256 dc_out = _mm256_add_ps(
+                        _mm256_sub_ps(filter[ch], dc_x[ch]),
+                        _mm256_mul_ps(dc_r, dc_y[ch]));
+                    dc_x[ch] = filter[ch];
+                    dc_y[ch] = dc_out;
+                    tile[t] = _mm256_add_ps(_mm256_set1_ps(x[ch][t]),
+                        _mm256_mul_ps(_mm256_set1_ps(feedbacks[t]), dc_out));
+                }
+                freeverb_transpose8(tile);
+                for (std::size_t c = 0; c < 8; ++c)
+                    _mm256_storeu_ps(comb[ch][c] + j, tile[c]);
+                // No line wraps inside this tile; every delay exceeds eight
+                // samples, so allpass lanes are independent time samples.
+                const __m256 gain = _mm256_set1_ps(0.5f);
+                for (std::size_t a = 0; a < FreeverbState::NUM_ALLPASSES; ++a) {
+                    float* buffer = allpass[ch][a] + j;
+                    const __m256 delayed = _mm256_loadu_ps(buffer);
+                    const __m256 output = _mm256_sub_ps(delayed, _mm256_mul_ps(gain, y));
+                    _mm256_storeu_ps(buffer,
+                        _mm256_add_ps(y, _mm256_mul_ps(gain, output)));
+                    y = output;
+                }
+                _mm256_storeu_ps(y_lr[ch], y);
+            }
+            for (std::size_t t = 0; t < 8; ++t) {
+                out_l[i + t] = drywet::mix(x[0][t], y_lr[0][t], dry[t], wet[t]);
+                out_r[i + t] = drywet::mix(x[1][t], y_lr[1][t], dry[t], wet[t]);
+            }
+        }
+        for (; j < count; ++j) {
+            const std::size_t i = base + j;
+            const float x_l = input[i];
+            const float x_r = stereo_in ? input_r[i] : x_l;
+            const float dry = drywet::coeff(dry_buf, i, dry_const);
+            const float wet = drywet::coeff(wet_buf, i, wet_const);
+            const float room = std::clamp(room_size[i], 0.0f, 1.0f);
+            const float damp = std::clamp(damping[i], 0.0f, 1.0f);
+            const float scale = room_scale_in[i] > 0.0f
+                ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
+            const float offset = room_offset_in[i] > 0.0f
+               
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
   No approximations are allowed for these opcodes.
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T2112Z_gpt-6-astra_freeverb/check.sh`. It builds, checks the allowlist and bit-identity for
   every opcode, prints compiler vectoriser remarks for `cedar/include/cedar/opcodes/reverbs.hpp`, and runs a
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
| 1 | rejected: `empty patch` — no files changed since the last accepted state | bottleneck unverified -> no change yet (check not run; awaiting permission to read source) | — |
| 2 | ACCEPTED | scalar comb feedback and modulo overhead -> vectorize eight combs with AVX2 and conditional wraparound (check passed; 2.08x faster) | reverb_freeverb 2.115x |
| 3 | ACCEPTED | per-sample delay-position bookkeeping -> process spans ending at the next wrap and update positions once per span (check passed; 1.37x faster) | reverb_freeverb 1.355x |
| 4 | ACCEPTED | per-sample gather/scatter and scalar diffusion -> eight-frame AVX2 transposes and time-vectorized allpasses (check passed; 1.53x faster) | reverb_freeverb 1.535x |
| 5 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_reverb_freeverb 1.015x (band 4.7%) | scalar coefficient preparation -> compute eight feedback and damping coefficients with AVX2 (check passed; 0.99x speed, no improvement) | — |
| 6 | rejected: `speed` — slower than baseline: op_reverb_freeverb 0.898x | serial feedback latency -> interleave independent L/R AVX2 recurrences (check passed exact equality; 1523.29 -> 1750.91 ns/block, 0.87x) | — |

Full verifier output for the latest rejection (attempt 6, gate `speed`):
```
slower than baseline: op_reverb_freeverb 0.898x
```


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
#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::reverb_freeverb_avx2(ctx, inst);
        return;
    }
#endif
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
| `op_reverb_freeverb` | 6784.81 | 2780.93 | 2.44x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/reverbs.hpp b/cedar/include/cedar/opcodes/reverbs.hpp
index 0d74114..7f7fb5a 100644
--- a/cedar/include/cedar/opcodes/reverbs.hpp
+++ b/cedar/include/cedar/opcodes/reverbs.hpp
@@ -1,5 +1,7 @@
 #pragma once
 
+#include "../dsp/simd.hpp"
+
 #include "../vm/context.hpp"
 #include "../vm/instruction.hpp"
 #include "../dsp/constants.hpp"
@@ -33,8 +35,126 @@ constexpr float FREEVERB_ROOM_OFFSET_DEFAULT = 0.7f;
 // Classic algorithm: 8 parallel lowpass-feedback comb filters summed,
 // then through 4 series allpass filters. Creates lush, dense reverb.
 
+namespace kernels {
+#if CEDAR_SIMD_X86
+CEDAR_TARGET_AVX2 inline void reverb_freeverb_avx2(ExecutionContext& ctx,
+                                                   const Instruction& inst) noexcept {
+    float* out_l = ctx.buffers->get(inst.out_buffer);
+    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
+    const float* input = ctx.buffers->get(inst.inputs[0]);
+    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
+    const float* input_r = stereo_in
+        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
+        : nullptr;
+    const float* room_size = ctx.buffers->get(inst.inputs[1]);
+    const float* damping = ctx.buffers->get(inst.inputs[2]);
+    const float* room_scale_in = ctx.buffers->get(inst.inputs[3]);
+    const float* room_offset_in = ctx.buffers->get(inst.inputs[4]);
+    auto& state = ctx.states->get_or_create<FreeverbState>(inst.state_id);
+
+    state.ensure_buffers(ctx.arena);
+    if (!state.buffers_ready()) {
+        CEDAR_LOG_ONCE("[CEDAR] audio arena exhausted: reverb(freeverb) → dry passthrough\n");
+        drywet::passthrough_stereo(input, input_r, stereo_in, out_l, out_r, BLOCK_SIZE);
+        return;
+    }
+
+    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
+    const float* dry_buf = nullptr; float dry_const = 1.0f;
+    const float* wet_buf = nullptr; float wet_const = 0.5f;
+    if (ext) {
+        const auto& d = ext->params[0];
+        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
+        const auto& w = ext->params[1];
+        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
+    }
+
+    constexpr float ALLPASS_GAIN = 0.5f;
+    alignas(32) float delayed_values[FreeverbState::NUM_COMBS];
+    alignas(32) float feedback_values[FreeverbState::NUM_COMBS];
+
+    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
+        const float x_l = input[i];
+        const float x_r = stereo_in ? input_r[i] : x_l;
+        const float dry = drywet::coeff(dry_buf, i, dry_const);
+        const float wet = drywet::coeff(wet_buf, i, wet_const);
+        const float room = std::clamp(room_size[i], 0.0f, 1.0f);
+        const float damp = std::clamp(damping[i], 0.0f, 1.0f);
+        const float room_scale = room_scale_in[i] > 0.0f
+            ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
+        const float room_offset = room_offset_in[i] > 0.0f
+            ? room_offset_in[i] : FREEVERB_ROOM_OFFSET_DEFAULT;
+        const float feedback = room * room_scale + room_offset;
+
+        float y_lr[2] = {0.0f, 0.0f};
+        for (std::size_t ch = 0; ch < 2; ++ch) {
+            const float x = ch == 0 ? x_l : x_r;
+            float comb_sum = 0.0f;
+            for (std::size_t c = 0; c < FreeverbState::NUM_COMBS; ++c) {
+                const std::size_t pos = state.comb_pos[ch][c];
+                const float delayed = state.comb_buffers[ch][c][pos];
+                delayed_values[c] = delayed;
+                comb_sum += delayed;
+            }
+
+            const __m256 delayed = _mm256_load_ps(delayed_values);
+            const __m256 damp_v = _mm256_set1_ps(damp);
+            const __m256 one_minus_damp_v = _mm256_set1_ps(1.0f - damp);
+            const __m256 old_filter = _mm256_loadu_ps(state.comb_filter_state[ch]);
+            const __m256 filtered = _mm256_add_ps(
+                _mm256_mul_ps(delayed, one_minus_damp_v),
+                _mm256_mul_ps(old_filter, damp_v));
+            const __m256 old_x1 = _mm256_loadu_ps(state.dc_x1[ch]);
+            const __m256 old_y1 = _mm256_loadu_ps(state.dc_y1[ch]);
+            const __m256 dc_out = _mm256_add_ps(
+                _mm256_sub_ps(filtered, old_x1),
+                _mm256_mul_ps(_mm256_set1_ps(DC_BLOCKER_R), old_y1));
+            _mm256_storeu_ps(state.comb_filter_state[ch], filtered);
+            _mm256_storeu_ps(state.dc_x1[ch], filtered);
+            _mm256_storeu_ps(state.dc_y1[ch], dc_out);
+            const __m256 writes = _mm256_add_ps(
+                _mm256_set1_ps(x),
+                _mm256_mul_ps(_mm256_set1_ps(feedback), dc_out));
+            _mm256_store_ps(feedback_values, writes);
+
+            for (std::size_t c = 0; c < FreeverbState::NUM_COMBS; ++c) {
+                std::size_t pos = state.comb_pos[ch][c];
+                state.comb_buffers[ch][c][pos] = feedback_values[c];
+                ++pos;
+                if (pos == FreeverbState::COMB_SIZES_LR[ch][c]) pos = 0;
+                state.comb_pos[ch][c] = pos;
+            }
+
+            float y = comb_sum;
+            for (std::size_t a = 0; a < FreeverbState::NUM_ALLPASSES; ++a) {
+                float* buffer = state.allpass_buffers[ch][a];
+                std::size_t pos = state.allpass_pos[ch][a];
+                const float delayed_ap = buffer[pos];
+                const float output = delayed_ap - ALLPASS_GAIN * y;
+                buffer[pos] = y + ALLPASS_GAIN * output;
+                ++pos;
+                if (pos == FreeverbState::ALLPASS_SIZES_LR[ch][a]) pos = 0;
+                state.allpass_pos[ch][a] = pos;
+                y = output;
+            }
+            y_lr[ch] = y;
+        }
+
+        out_l[i] = drywet::mix(x_l, y_lr[0], dry, wet);
+        out_r[i] = drywet::mix(x_r, y_lr[1], dry, wet);
+    }
+}
+#endif
+}  // namespace kernels
+
 [[gnu::always_inline]]
 inline void op_reverb_freeverb(ExecutionContext& ctx, const Instruction& inst) {
+#if CEDAR_SIMD_X86
+    if (simd::has(simd::Isa::Avx2)) {
+        kernels::reverb_freeverb_avx2(ctx, inst);
+        return;
+    }
+#endif
     // Stereo-native: writes out_buffer (L) and out_buffer+1 (R) in one call.
     // When STEREO_INPUT is set in inst.flags, the primary signal input is a
     // stereo pair (inputs[0] = L, inputs[0]+1 = R). When unset, the mono input
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-27T1533Z_gpt-5-6-sol_freeverb/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | parallel comb-feedback arithmetic and delay-index wrapping -> vectorized eight comb states per channel with AVX2 and exact increment-and-wrap indexing (check passed exact equality and measured 2.70x s | reverb_freeverb 2.432x |
| 2 | rejected: `empty patch` — no files changed since the last accepted state | repeated ring-index state traffic -> cached indices locally for the block, then reverted it (exact equality passed; benchmark regressed 2761.78 to 2783.34 ns/block, 0.99x) | — |

Full verifier output for the latest rejection (attempt 2, gate `empty patch`):
```
no files changed since the last accepted state
```


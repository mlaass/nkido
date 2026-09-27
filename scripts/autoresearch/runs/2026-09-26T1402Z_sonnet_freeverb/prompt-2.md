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
        freeverb_kernels::freeverb_avx2(state, input, stereo_in ? input_r : input,
                                        room_size, damping, room_scale_in, room_offset_in,
                                        dry_buf, dry_const, wet_buf, wet_const,
                                        out_l, out_r);
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
| `op_reverb_freeverb` | 6784.81 | 2150.45 | 3.16x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/reverbs.hpp b/cedar/include/cedar/opcodes/reverbs.hpp
index 0d74114..f34eb77 100644
--- a/cedar/include/cedar/opcodes/reverbs.hpp
+++ b/cedar/include/cedar/opcodes/reverbs.hpp
@@ -7,6 +7,8 @@
 #include "dsp_utils.hpp"
 #include "drywet.hpp"
 #include "../util/log_once.hpp"
+#include "../dsp/simd.hpp"
+#include <cstring>
 #include <cmath>
 #include <algorithm>
 
@@ -33,6 +35,194 @@ constexpr float FREEVERB_ROOM_OFFSET_DEFAULT = 0.7f;
 // Classic algorithm: 8 parallel lowpass-feedback comb filters summed,
 // then through 4 series allpass filters. Creates lush, dense reverb.
 
+namespace freeverb_kernels {
+#if CEDAR_SIMD_X86
+
+// In-place 8x8 float transpose.
+[[gnu::always_inline]] CEDAR_TARGET_AVX2 inline void transpose8x8(__m256 (&r)[8]) noexcept {
+    const __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]);
+    const __m256 t1 = _mm256_unpackhi_ps(r[0], r[1]);
+    const __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]);
+    const __m256 t3 = _mm256_unpackhi_ps(r[2], r[3]);
+    const __m256 t4 = _mm256_unpacklo_ps(r[4], r[5]);
+    const __m256 t5 = _mm256_unpackhi_ps(r[4], r[5]);
+    const __m256 t6 = _mm256_unpacklo_ps(r[6], r[7]);
+    const __m256 t7 = _mm256_unpackhi_ps(r[6], r[7]);
+    const __m256 u0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
+    const __m256 u1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
+    const __m256 u2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
+    const __m256 u3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
+    const __m256 u4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
+    const __m256 u5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
+    const __m256 u6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
+    const __m256 u7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));
+    r[0] = _mm256_permute2f128_ps(u0, u4, 0x20);
+    r[1] = _mm256_permute2f128_ps(u1, u5, 0x20);
+    r[2] = _mm256_permute2f128_ps(u2, u6, 0x20);
+    r[3] = _mm256_permute2f128_ps(u3, u7, 0x20);
+    r[4] = _mm256_permute2f128_ps(u0, u4, 0x31);
+    r[5] = _mm256_permute2f128_ps(u1, u5, 0x31);
+    r[6] = _mm256_permute2f128_ps(u2, u6, 0x31);
+    r[7] = _mm256_permute2f128_ps(u3, u7, 0x31);
+}
+
+// One allpass stage over a contiguous run of the delay line. y holds the stage
+// input on entry and the stage output on exit. Elements are independent
+// because the delay is longer than a block.
+CEDAR_TARGET_AVX2 inline void allpass_run(float* p, float* y, std::size_t len) noexcept {
+    const __m256 g = _mm256_set1_ps(0.5f);
+    std::size_t j = 0;
+    for (; j + 8 <= len; j += 8) {
+        const __m256 d = _mm256_loadu_ps(p + j);
+        const __m256 yy = _mm256_loadu_ps(y + j);
+        const __m256 o = _mm256_sub_ps(d, _mm256_mul_ps(g, yy));
+        _mm256_storeu_ps(p + j, _mm256_add_ps(yy, _mm256_mul_ps(g, o)));
+        _mm256_storeu_ps(y + j, o);
+    }
+    for (; j < len; ++j) {
+        const float d = p[j];
+        const float yy = y[j];
+        const float o = d - 0.5f * yy;
+        p[j] = yy + 0.5f * o;
+        y[j] = o;
+    }
+}
+
+CEDAR_TARGET_AVX2 inline void freeverb_avx2(
+    FreeverbState& st, const float* in_l, const float* in_r,
+    const float* room_size, const float* damping,
+    const float* room_scale_in, const float* room_offset_in,
+    const float* dry_buf, float dry_const, const float* wet_buf, float wet_const,
+    float* out_l, float* out_r) noexcept {
+    constexpr std::size_t N = BLOCK_SIZE;
+    constexpr std::size_t NC = FreeverbState::NUM_COMBS;
+    constexpr std::size_t NA = FreeverbState::NUM_ALLPASSES;
+    static_assert(NC == 8 && N % 8 == 0, "freeverb kernel layout");
+    static_assert(FreeverbState::COMB_SIZES_LR[0][7] > N &&
+                  FreeverbState::ALLPASS_SIZES_LR[0][0] > N,
+                  "delay lines must be longer than a block");
+
+    alignas(32) float fb[N];
+    alignas(32) float damp[N];
+    alignas(32) float omd[N];
+    alignas(32) float dl[2][NC][N];
+    alignas(32) float ys[2][N];
+
+    for (std::size_t i = 0; i < N; ++i) {
+        const float room = std::clamp(room_size[i], 0.0f, 1.0f);
+        const float d = std::clamp(damping[i], 0.0f, 1.0f);
+        const float room_scale = room_scale_in[i] > 0.0f ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
+        const float room_offset = room_offset_in[i] > 0.0f ? room_offset_in[i] : FREEVERB_ROOM_OFFSET_DEFAULT;
+        fb[i] = room * room_scale + room_offset;
+        damp[i] = d;
+        omd[i] = 1.0f - d;
+    }
+
+    // Read this block's worth of delayed samples from every comb line.
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        for (std::size_t c = 0; c < NC; ++c) {
+            const float* buf = st.comb_buffers[ch][c];
+            const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
+            const std::size_t pos = st.comb_pos[ch][c];
+            const std::size_t first = std::min(N, size - pos);
+            std::memcpy(dl[ch][c], buf + pos, first * sizeof(float));
+            if (first < N) std::memcpy(dl[ch][c] + first, buf, (N - first) * sizeof(float));
+        }
+    }
+
+    const __m256 rv = _mm256_set1_ps(DC_BLOCKER_R);
+    __m256 fs[2], x1[2], y1[2];
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        fs[ch] = _mm256_loadu_ps(st.comb_filter_state[ch]);
+        x1[ch] = _mm256_loadu_ps(st.dc_x1[ch]);
+        y1[ch] = _mm256_loadu_ps(st.dc_y1[ch]);
+    }
+    const float* xin[2] = {in_l, in_r};
+
+    for (std::size_t n = 0; n < N; n += 8) {
+        __m256 v[2][8];
+        for (std::size_t ch = 0; ch < 2; ++ch) {
+            __m256 r[8];
+            for (std::size_t c = 0; c < NC; ++c) r[c] = _mm256_load_ps(dl[ch][c] + n);
+            __m256 s = _mm256_setzero_ps();
+            for (std::size_t c = 0; c < NC; ++c) s = _mm256_add_ps(s, r[c]);
+            _mm256_store_ps(ys[ch] + n, s);
+            transpose8x8(r);
+            for (std::size_t t = 0; t < 8; ++t) v[ch][t] = r[t];
+        }
+        __m256 w[2][8];
+        for (std::size_t t = 0; t < 8; ++t) {
+            const __m256 bd = _mm256_broadcast_ss(damp + n + t);
+            const __m256 bo = _mm256_broadcast_ss(omd + n + t);
+            const __m256 bf = _mm256_broadcast_ss(fb + n + t);
+            for (std::size_t ch = 0; ch < 2; ++ch) {
+                const __m256 x = _mm256_broadcast_ss(xin[ch] + n + t);
+                fs[ch] = _mm256_add_ps(_mm256_mul_ps(v[ch][t], bo), _mm256_mul_ps(fs[ch], bd));
+                const __m256 dc_out = _mm256_add_ps(_mm256_sub_ps(fs[ch], x1[ch]),
+                                                    _mm256_mul_ps(rv, y1[ch]));
+                x1[ch] = fs[ch];
+                y1[ch] = dc_out;
+                w[ch][t] = _mm256_add_ps(x, _mm256_mul_ps(bf, dc_out));
+            }
+        }
+        for (std::size_t ch = 0; ch < 2; ++ch) {
+            __m256 r[8];
+            for (std::size_t t = 0; t < 8; ++t) r[t] = w[ch][t];
+            transpose8x8(r);
+            for (std::size_t c = 0; c < NC; ++c) _mm256_store_ps(dl[ch][c] + n, r[c]);
+        }
+    }
+
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        _mm256_storeu_ps(st.comb_filter_state[ch], fs[ch]);
+        _mm256_storeu_ps(st.dc_x1[ch], x1[ch]);
+        _mm256_storeu_ps(st.dc_y1[ch], y1[ch]);
+    }
+
+    // Write the new feedback samples back into the comb lines.
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        for (std::size_t c = 0; c < NC; ++c) {
+            float* buf = st.comb_buffers[ch][c];
+            const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
+            const std::size_t pos = st.comb_pos[ch][c];
+            const std::size_t first = std::min(N, size - pos);
+            std::memcpy(buf + pos, dl[ch][c], first * sizeof(float));
+            if (first < N) std::memcpy(buf, dl[ch][c] + first, (N - first) * sizeof(float));
+            const std::size_t np = pos + N;
+            st.comb_pos[ch][c] = np >= size ? np - size : n
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1402Z_sonnet_freeverb/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | per-sample modulo divisions and serial dependencies -> AVX2 kernel that batches delay-line reads/writes per block, runs the 16 comb lanes in AVX2 via 8x8 transposes, and vectorises the four allpasses  | reverb_freeverb 3.436x |


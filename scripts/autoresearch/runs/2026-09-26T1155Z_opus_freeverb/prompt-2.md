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
        kernels::freeverb_avx2(state, input, stereo_in ? input_r : input,
                               room_size, damping, room_scale_in, room_offset_in,
                               dry_buf, dry_const, wet_buf, wet_const, out_l, out_r);
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
| `op_reverb_freeverb` | 6784.81 | 2096.29 | 3.24x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/reverbs.hpp b/cedar/include/cedar/opcodes/reverbs.hpp
index 0d74114..14946e7 100644
--- a/cedar/include/cedar/opcodes/reverbs.hpp
+++ b/cedar/include/cedar/opcodes/reverbs.hpp
@@ -7,8 +7,10 @@
 #include "dsp_utils.hpp"
 #include "drywet.hpp"
 #include "../util/log_once.hpp"
+#include "../dsp/simd.hpp"
 #include <cmath>
 #include <algorithm>
+#include <cstring>
 
 namespace cedar {
 
@@ -33,6 +35,183 @@ constexpr float FREEVERB_ROOM_OFFSET_DEFAULT = 0.7f;
 // Classic algorithm: 8 parallel lowpass-feedback comb filters summed,
 // then through 4 series allpass filters. Creates lush, dense reverb.
 
+namespace kernels {
+
+// Copy n samples of a circular delay line starting at pos (handles wrap).
+[[gnu::always_inline]]
+inline void ring_read(const float* buf, std::size_t size, std::size_t pos,
+                      float* dst, std::size_t n) noexcept {
+    std::size_t n1 = size - pos < n ? size - pos : n;
+    std::memcpy(dst, buf + pos, n1 * sizeof(float));
+    std::memcpy(dst + n1, buf, (n - n1) * sizeof(float));
+}
+
+[[gnu::always_inline]]
+inline void ring_write(float* buf, std::size_t size, std::size_t pos,
+                       const float* src, std::size_t n) noexcept {
+    std::size_t n1 = size - pos < n ? size - pos : n;
+    std::memcpy(buf + pos, src, n1 * sizeof(float));
+    std::memcpy(buf, src + n1, (n - n1) * sizeof(float));
+}
+
+#if CEDAR_SIMD_X86
+// In-place-safe 8x8 transpose: rows src[r*src_stride .. +8] -> dst rows.
+CEDAR_TARGET_AVX2 inline void transpose8x8_avx2(const float* src, std::size_t src_stride,
+                                                float* dst, std::size_t dst_stride) noexcept {
+    __m256 r0 = _mm256_load_ps(src + 0 * src_stride);
+    __m256 r1 = _mm256_load_ps(src + 1 * src_stride);
+    __m256 r2 = _mm256_load_ps(src + 2 * src_stride);
+    __m256 r3 = _mm256_load_ps(src + 3 * src_stride);
+    __m256 r4 = _mm256_load_ps(src + 4 * src_stride);
+    __m256 r5 = _mm256_load_ps(src + 5 * src_stride);
+    __m256 r6 = _mm256_load_ps(src + 6 * src_stride);
+    __m256 r7 = _mm256_load_ps(src + 7 * src_stride);
+    __m256 t0 = _mm256_unpacklo_ps(r0, r1);
+    __m256 t1 = _mm256_unpackhi_ps(r0, r1);
+    __m256 t2 = _mm256_unpacklo_ps(r2, r3);
+    __m256 t3 = _mm256_unpackhi_ps(r2, r3);
+    __m256 t4 = _mm256_unpacklo_ps(r4, r5);
+    __m256 t5 = _mm256_unpackhi_ps(r4, r5);
+    __m256 t6 = _mm256_unpacklo_ps(r6, r7);
+    __m256 t7 = _mm256_unpackhi_ps(r6, r7);
+    __m256 s0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
+    __m256 s1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
+    __m256 s2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
+    __m256 s3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
+    __m256 s4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
+    __m256 s5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
+    __m256 s6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
+    __m256 s7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));
+    _mm256_store_ps(dst + 0 * dst_stride, _mm256_permute2f128_ps(s0, s4, 0x20));
+    _mm256_store_ps(dst + 1 * dst_stride, _mm256_permute2f128_ps(s1, s5, 0x20));
+    _mm256_store_ps(dst + 2 * dst_stride, _mm256_permute2f128_ps(s2, s6, 0x20));
+    _mm256_store_ps(dst + 3 * dst_stride, _mm256_permute2f128_ps(s3, s7, 0x20));
+    _mm256_store_ps(dst + 4 * dst_stride, _mm256_permute2f128_ps(s0, s4, 0x31));
+    _mm256_store_ps(dst + 5 * dst_stride, _mm256_permute2f128_ps(s1, s5, 0x31));
+    _mm256_store_ps(dst + 6 * dst_stride, _mm256_permute2f128_ps(s2, s6, 0x31));
+    _mm256_store_ps(dst + 7 * dst_stride, _mm256_permute2f128_ps(s3, s7, 0x31));
+}
+
+// Every comb/allpass delay (>= 225 samples) exceeds BLOCK_SIZE, so a block's
+// reads never observe its own writes: read the whole block of delayed samples
+// up front, run the 8 comb recurrences of each lane as one 8-wide vector
+// (same per-lane op order as scalar), then write the block back.
+CEDAR_TARGET_AVX2 inline void freeverb_avx2(
+    FreeverbState& state, const float* x_l, const float* x_r,
+    const float* room_size, const float* damping,
+    const float* room_scale_in, const float* room_offset_in,
+    const float* dry_buf, float dry_const, const float* wet_buf, float wet_const,
+    float* out_l, float* out_r) noexcept {
+    constexpr std::size_t N = BLOCK_SIZE;
+    constexpr std::size_t NC = FreeverbState::NUM_COMBS;
+    static_assert(NC == 8, "freeverb_avx2 assumes 8 combs per lane");
+    alignas(32) float damp_a[N];
+    alignas(32) float omd_a[N];
+    alignas(32) float fb_a[N];
+    alignas(32) float lin[NC][N];      // [comb][sample]
+    alignas(32) float tr[2][N][NC];    // [lane][sample][comb]
+    alignas(32) float ysum[2][N];
+    alignas(32) float apd[N];
+
+    for (std::size_t i = 0; i < N; ++i) {
+        float room = std::clamp(room_size[i], 0.0f, 1.0f);
+        float damp = std::clamp(damping[i], 0.0f, 1.0f);
+        float room_scale = room_scale_in[i] > 0.0f ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
+        float room_offset = room_offset_in[i] > 0.0f ? room_offset_in[i] : FREEVERB_ROOM_OFFSET_DEFAULT;
+        damp_a[i] = damp;
+        omd_a[i] = 1.0f - damp;
+        fb_a[i] = room * room_scale + room_offset;
+    }
+
+    // Gather delayed comb outputs, sum them in scalar order, transpose.
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        for (std::size_t c = 0; c < NC; ++c)
+            ring_read(state.comb_buffers[ch][c], FreeverbState::COMB_SIZES_LR[ch][c],
+                      state.comb_pos[ch][c], lin[c], N);
+        for (std::size_t i = 0; i < N; i += 8) {
+            __m256 s = _mm256_setzero_ps();
+            for (std::size_t c = 0; c < NC; ++c) s = _mm256_add_ps(s, _mm256_load_ps(lin[c] + i));
+            _mm256_store_ps(ysum[ch] + i, s);
+            transpose8x8_avx2(&lin[0][i], N, &tr[ch][i][0], NC);
+        }
+    }
+
+    // Comb recurrences: both lanes interleaved for ILP.
+    const __m256 R = _mm256_set1_ps(DC_BLOCKER_R);
+    __m256 fs0 = _mm256_loadu_ps(state.comb_filter_state[0]);
+    __m256 fs1 = _mm256_loadu_ps(state.comb_filter_state[1]);
+    __m256 x10 = _mm256_loadu_ps(state.dc_x1[0]);
+    __m256 x11 = _mm256_loadu_ps(state.dc_x1[1]);
+    __m256 y10 = _mm256_loadu_ps(state.dc_y1[0]);
+    __m256 y11 = _mm256_loadu_ps(state.dc_y1[1]);
+    for (std::size_t i = 0; i < N; ++i) {
+        const __m256 dmp = _mm256_broadcast_ss(damp_a + i);
+        const __m256 omd = _mm256_broadcast_ss(omd_a + i);
+        const __m256 fb = _mm256_broadcast_ss(fb_a + i);
+        const __m256 d0 = _mm256_load_ps(tr[0][i]);
+        const __m256 d1 = _mm256_load_ps(tr[1][i]);
+        fs0 = _mm256_add_ps(_mm256_mul_ps(d0, omd), _mm256_mul_ps(fs0, dmp));
+        fs1 = _mm256_add_ps(_mm256_mul_ps(d1, omd), _mm256_mul_ps(fs1, dmp));
+        __m256 o0 = _mm256_add_ps(_mm256_sub_ps(fs0, x10), _mm256_mul_ps(R, y10));
+        __m256 o1 = _mm256_add_ps(_mm256_sub_ps(fs1, x11), _mm256_mul_ps(R, y11));
+        x10 = fs0; x11 = fs1;
+        y10 = o0; y11 = o1;
+        _mm256_store_ps(tr[0][i], _mm256_add_ps(_mm256_broadcast_ss(x_l + i), _mm256_mul_ps(fb, o0)));
+        _mm256_store_ps(tr[1][i], _mm256_add_ps(_mm256_broadcast_ss(x_r + i), _mm256_mul_ps(fb, o1)));
+    }
+    _mm256_storeu_ps(state.comb_filter_state[0], fs0);
+    _mm256_storeu_ps(state.comb_filter_state[1], fs1);
+    _mm256_storeu_ps(state.dc_x1[0], x10);
+    _mm256_storeu_ps(state.dc_x1[1], x11);
+    _mm256_storeu_ps(state.dc_y1[0], y10);
+    _mm256_storeu_ps(state.dc_y1[1], y11);
+
+    // Transpose back and write comb lines.
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        for (std::size_t i = 0; i < N; i += 8)
+            transpose8x8_avx2(&tr[ch][i][0], NC, &lin[0][i], N);
+        for (std::size_t c = 0; c < NC; ++c) {
+            const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
+            const std::size_t pos = state.comb_pos[ch][c];
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
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T1155Z_opus_freeverb/check.sh`. It builds, checks the allowlist and bit-identity for
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
| 1 | ACCEPTED | 16 serial comb recurrences per sample with a modulo on every delay line, all delays longer than the block -> AVX2 kernel that reads each delay line's block up front, transposes so the 8 combs are one  | reverb_freeverb 3.203x |


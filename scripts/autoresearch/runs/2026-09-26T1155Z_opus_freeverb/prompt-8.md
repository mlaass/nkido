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
| `op_reverb_freeverb` | 6784.81 | 1212.87 | 5.59x |

## Accepted so far (already in the tree)

```diff
diff --git a/cedar/include/cedar/opcodes/reverbs.hpp b/cedar/include/cedar/opcodes/reverbs.hpp
index 0d74114..0bc8234 100644
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
 
@@ -33,6 +35,271 @@ constexpr float FREEVERB_ROOM_OFFSET_DEFAULT = 0.7f;
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
+// Two independent 4x4 transposes, one per 128-bit lane. The cross-lane half
+// of an 8x8 transpose is done by 128-bit loads/stores instead of shuffles.
+[[gnu::always_inline]] CEDAR_TARGET_AVX2 inline void transpose4x4_lanes_avx2(__m256 (&r)[4]) noexcept {
+    __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]);
+    __m256 t1 = _mm256_unpackhi_ps(r[0], r[1]);
+    __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]);
+    __m256 t3 = _mm256_unpackhi_ps(r[2], r[3]);
+    r[0] = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
+    r[1] = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
+    r[2] = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
+    r[3] = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
+}
+
+// [lo | hi] from two 128-bit loads.
+[[gnu::always_inline]] CEDAR_TARGET_AVX2 inline __m256 load_2x128_avx2(const float* lo,
+                                                                       const float* hi) noexcept {
+    return _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(lo)), _mm_loadu_ps(hi), 1);
+}
+
+// Ring position of block offset i for a line of `size` (> BLOCK_SIZE).
+[[gnu::always_inline]] inline std::size_t ring_at(std::size_t pos, std::size_t i,
+                                                  std::size_t size) noexcept {
+    std::size_t p = pos + i;
+    return p >= size ? p - size : p;
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
+    alignas(32) float wrap_in[NC][8];  // chunks straddling a ring's wrap point
+    alignas(32) float wrap_out[NC][8];
+    alignas(32) float apd[8];
+
+    {
+        // max_ps(0, v) / min_ps(1, v) return v for NaN, matching std::clamp.
+        const __m256 zero = _mm256_setzero_ps();
+        const __m256 one = _mm256_set1_ps(1.0f);
+        const __m256 scale_def = _mm256_set1_ps(FREEVERB_ROOM_SCALE_DEFAULT);
+        const __m256 offset_def = _mm256_set1_ps(FREEVERB_ROOM_OFFSET_DEFAULT);
+        for (std::size_t i = 0; i < N; i += 8) {
+            __m256 room = _mm256_min_ps(one, _mm256_max_ps(zero, _mm256_loadu_ps(room_size + i)));
+            __m256 damp = _mm256_min_ps(one, _mm256_max_ps(zero, _mm256_loadu_ps(damping + i)));
+            __m256 rs = _mm256_loadu_ps(room_scale_in + i);
+            __m256 ro = _mm256_loadu_ps(room_offset_in + i);
+            rs = _mm256_blendv_ps(scale_def, rs, _mm256_cmp_ps(rs, zero, _CMP_GT_OQ));
+            ro = _mm256_blendv_ps(offset_def, ro, _mm256_cmp_ps(ro, zero, _CMP_GT_OQ));
+            _mm256_store_ps(damp_a + i, damp);
+            _mm256_store_ps(omd_a + i, _mm256_sub_ps(one, damp));
+            _mm256_store_ps(fb_a + i, _mm256_add_ps(_mm256_mul_ps(room, rs), ro));
+        }
+    }
+
+    // One pass over 8-sample chunks: gather the delayed comb outputs, run the
+    // allpasses + mix on their sum, step the 8-wide comb recurrences, and write
+    // the comb inputs back. Fusing lets the transposes overlap the recurrence
+    // latency. Only chunks straddling a wrap point are staged.
+    constexpr std::size_t NA = FreeverbState::NUM_ALLPASSES;
+    const __m256 g = _mm256_set1_ps(0.5f);
+    const __m256 R = _mm256_set1_ps(DC_BLOCKER_R);
+    float* cb[2][NC];
+    std::size_t cpos[2][NC];
+    float* ap_buf[2][NA];
+    std::size_t ap_pos[2][NA];
+    // cbase/apbase + i addresses block offset i while no wrap has been passed;
+    // each ring wraps at most once per block (size > N). Per-chunk bitmasks
+    // (bit ch*8+c for combs, 16+ch*4+a for allpasses) say which rings straddle
+    // their wrap point in that chunk and which rebase (-= size) after it.
+    float* cbase[2][NC];
+    float* apbase[2][NA];
+    std::uint32_t straddle[N / 8] = {};
+    std::uint32_t rebase[N / 8] = {};
+    auto mark = [&](std::size_t pos, std::size_t size, unsigned bit) {
+        const std::size_t rem = size - pos;  // samples until wrap, >= 1
+        if (rem <= N) {
+            rebase[(rem - 1) / 8] |= 1u << bit;
+            if (rem % 8 != 0) straddle[(rem - 1) / 8] |= 1u << bit;
+        }
+    };
+    for (std::size_t ch = 0; ch < 2; ++ch) {
+        for (std::size_t c = 0; c < NC; ++c) {
+            cb[ch][c] = state.comb_buffers[ch][c];
+            cpos[ch][c] = state.comb_pos[ch][c];
+            cbase[ch][c] = cb[ch][c] + cpos[ch][c];
+            mark(cpos[ch][c], FreeverbState::COMB_SIZES_LR[ch][c], unsigned(ch * NC + c));
+        }
+        for (std::size_t a = 0; a < NA; ++a) {
+            ap_buf[ch][a] = state.allpass_buffers[ch][a];
+            ap_pos[ch][a] = state.allpass_pos[ch][a];
+            apbase[ch][a] = ap_buf[ch][a] + ap_pos[ch][a];
+            mark(ap_pos[ch][a], FreeverbState::ALLPASS_SIZES_LR[ch][a], unsigned(16 + ch * NA + a));
+        }
+    }
+    __m256 fs0 = _mm256_loadu_ps(state.comb_filter_state[0]);
+    __m256 fs1 = _mm256_loadu_ps(state.comb_filter_state[1]);
+    __m256 x10 = _mm256_loadu_ps(state.dc_x1[0]);
+    __m256 x11 = _mm256_loadu_ps(state.dc_x1[1]);
+    __m256 y10 = _mm256_loadu_ps(state.dc_y1[0]);
+    __m256 y11 = _mm256_loadu_ps(state.dc_y1[1]);
+
+    for (std::size_t i = 0; i < N; i += 8) {
+        const std::uint32_t sm = straddle[i / 8];
+        __m256 d[2][8];  // [lane][sample in chunk] -> 8 combs
+        __m256 ych[2];
+        for (std::size_t ch = 0; ch < 2; ++ch) {
+            const float* src[NC];
+            for (std::size_t c = 0; c < NC; ++c) {
+                src[c] = cbase[ch][c] + i;
+                if (sm & (1u << (ch * NC + c))) [[unlikely]] {
+                    const float* buf = cb[ch][c];
+                    const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
+                    const std::size_t p = cpos[ch][c] + i;  // < size before the wrap
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
| 2 | ACCEPTED | comb delay-line blocks were copied into a staging array and transposed on the way in and out of the 8-wide comb recurrence -> load rows straight from the delay lines into registers, transpose, and sto | reverb_freeverb 1.179x |
| 3 | ACCEPTED | allpass stage staged every ring through memcpy and made 8 separate passes over the block plus a mix pass -> fused the 4 series allpasses and the dry/wet mix into one per-8-sample-chunk pass, keeping y | reverb_freeverb 1.111x |
| 4 | ACCEPTED | comb recurrence latency chain and the 8x8 shuffle-port transposes ran in separate passes and couldn't overlap -> fused gather, allpass, 8-step recurrence, scatter and mix into one per-8-sample-chunk l | reverb_freeverb 1.135x |
| 5 | ACCEPTED | per-chunk wrap arithmetic for 24 delay lines (and double position computation for the combs) dominated the instruction count -> precompute per-chunk straddle/rebase bitmasks once per block so the fast | reverb_freeverb 1.176x |
| 6 | rejected: `speed` — no opcode improved by >= 5 % outside the noise band: op_reverb_freeverb 1.032x (band 3.3%) | total µop count per chunk (the checks ruled out shuffle ports, memory latency and pipelining overlap) -> moved the comb input update `x + fb*o` from the per-sample recurrence to after the back-transpo | — |
| 7 | rejected: `empty patch` — no files changed since the last accepted state | comb recurrence latency not overlapping next-chunk gather / delay-line L1 misses -> software-pipelined next-chunk gather+allpass between recurrence steps, then tried 2-chunk-ahead prefetch of all 24 r | — |

Full verifier output for the latest rejection (attempt 7, gate `empty patch`):
```
no files changed since the last accepted state
```


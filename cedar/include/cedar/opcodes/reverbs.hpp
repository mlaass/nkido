#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "dsp_utils.hpp"
#include "drywet.hpp"
#include "../util/log_once.hpp"
#include "../dsp/simd.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace cedar {

// DC blocker coefficient: R ≈ 0.997 gives ~23Hz cutoff at 48kHz
// y[n] = x[n] - x[n-1] + R * y[n-1]
constexpr float DC_BLOCKER_R = 0.997f;

// Default constants for Freeverb
constexpr float FREEVERB_ROOM_SCALE_DEFAULT = 0.28f;
constexpr float FREEVERB_ROOM_OFFSET_DEFAULT = 0.7f;

// ============================================================================
// REVERB_FREEVERB: Schroeder-Moorer Reverb (Freeverb Algorithm)
// ============================================================================
// in0: input signal
// in1: room size (0.0-1.0)
// in2: damping (0.0-1.0)
// in3: room_scale - density factor (default 0.28)
// in4: room_offset - decay baseline (default 0.7)
// rate: wet/dry mix (0-255 -> 0.0-1.0)
//
// Classic algorithm: 8 parallel lowpass-feedback comb filters summed,
// then through 4 series allpass filters. Creates lush, dense reverb.

namespace freeverb_kernels {
#if CEDAR_SIMD_X86

// In-place 4x4 float transpose within each 128-bit lane (self-inverse).
[[gnu::always_inline]] CEDAR_TARGET_AVX2 inline void transpose4x4_lanes(__m256 (&r)[4]) noexcept {
    const __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]);
    const __m256 t1 = _mm256_unpackhi_ps(r[0], r[1]);
    const __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]);
    const __m256 t3 = _mm256_unpackhi_ps(r[2], r[3]);
    r[0] = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
    r[1] = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
    r[2] = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
    r[3] = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
}

// One allpass stage over a contiguous run of the delay line. y holds the stage
// input on entry and the stage output on exit. Elements are independent
// because the delay is longer than a block.
CEDAR_TARGET_AVX2 inline void allpass_run(float* p, float* y, std::size_t len) noexcept {
    const __m256 g = _mm256_set1_ps(0.5f);
    std::size_t j = 0;
    for (; j + 8 <= len; j += 8) {
        const __m256 d = _mm256_loadu_ps(p + j);
        const __m256 yy = _mm256_loadu_ps(y + j);
        const __m256 o = _mm256_sub_ps(d, _mm256_mul_ps(g, yy));
        _mm256_storeu_ps(p + j, _mm256_add_ps(yy, _mm256_mul_ps(g, o)));
        _mm256_storeu_ps(y + j, o);
    }
    for (; j < len; ++j) {
        const float d = p[j];
        const float yy = y[j];
        const float o = d - 0.5f * yy;
        p[j] = yy + 0.5f * o;
        y[j] = o;
    }
}

CEDAR_TARGET_AVX2 inline void freeverb_avx2(
    FreeverbState& st, const float* in_l, const float* in_r,
    const float* room_size, const float* damping,
    const float* room_scale_in, const float* room_offset_in,
    const float* dry_buf, float dry_const, const float* wet_buf, float wet_const,
    float* out_l, float* out_r) noexcept {
    constexpr std::size_t N = BLOCK_SIZE;
    constexpr std::size_t NC = FreeverbState::NUM_COMBS;
    constexpr std::size_t NA = FreeverbState::NUM_ALLPASSES;
    static_assert(NC == 8 && N % 8 == 0, "freeverb kernel layout");
    static_assert(FreeverbState::COMB_SIZES_LR[0][7] > N &&
                  FreeverbState::ALLPASS_SIZES_LR[0][0] > N,
                  "delay lines must be longer than a block");

    alignas(32) float fb[N];
    alignas(32) float damp[N];
    alignas(32) float omd[N];
    alignas(32) float ys[2][N];
    alignas(32) float stage[2][NC][N];  // only touched when a comb line wraps inside the block

    for (std::size_t i = 0; i < N; ++i) {
        const float room = std::clamp(room_size[i], 0.0f, 1.0f);
        const float d = std::clamp(damping[i], 0.0f, 1.0f);
        const float room_scale = room_scale_in[i] > 0.0f ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
        const float room_offset = room_offset_in[i] > 0.0f ? room_offset_in[i] : FREEVERB_ROOM_OFFSET_DEFAULT;
        fb[i] = room * room_scale + room_offset;
        damp[i] = d;
        omd[i] = 1.0f - d;
    }

    // Each comb line is processed in place (read delayed, write feedback to the
    // same slot). Lines that wrap inside this block go through a staging copy.
    float* cp[2][NC];
    bool wrapped[2][NC];
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t c = 0; c < NC; ++c) {
            float* buf = st.comb_buffers[ch][c];
            const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
            const std::size_t pos = st.comb_pos[ch][c];
            if (pos + N <= size) {
                cp[ch][c] = buf + pos;
                wrapped[ch][c] = false;
            } else {
                const std::size_t first = size - pos;
                std::memcpy(stage[ch][c], buf + pos, first * sizeof(float));
                std::memcpy(stage[ch][c] + first, buf, (N - first) * sizeof(float));
                cp[ch][c] = stage[ch][c];
                wrapped[ch][c] = true;
            }
        }
    }

    const __m256 rv = _mm256_set1_ps(DC_BLOCKER_R);
    __m256 fs[2], x1[2], y1[2];
    for (std::size_t ch = 0; ch < 2; ++ch) {
        fs[ch] = _mm256_loadu_ps(st.comb_filter_state[ch]);
        x1[ch] = _mm256_loadu_ps(st.dc_x1[ch]);
        y1[ch] = _mm256_loadu_ps(st.dc_y1[ch]);
    }
    const float* xin[2] = {in_l, in_r};

    for (std::size_t n = 0; n < N; n += 8) {
        // Comb sums first: they read the delayed samples this iteration overwrites.
        for (std::size_t ch = 0; ch < 2; ++ch) {
            __m256 s = _mm256_setzero_ps();
            for (std::size_t c = 0; c < NC; ++c) s = _mm256_add_ps(s, _mm256_loadu_ps(cp[ch][c] + n));
            _mm256_store_ps(ys[ch] + n, s);
        }
        for (std::size_t h = 0; h < 2; ++h) {
            const std::size_t nh = n + 4 * h;
            // v[ch][t]: lanes 0..7 = combs 0..7 at time nh+t.
            __m256 v[2][4];
            for (std::size_t ch = 0; ch < 2; ++ch) {
                for (std::size_t i = 0; i < 4; ++i) {
                    v[ch][i] = _mm256_insertf128_ps(
                        _mm256_castps128_ps256(_mm_loadu_ps(cp[ch][i] + nh)),
                        _mm_loadu_ps(cp[ch][i + 4] + nh), 1);
                }
                transpose4x4_lanes(v[ch]);
            }
            for (std::size_t t = 0; t < 4; ++t) {
                const __m256 bd = _mm256_broadcast_ss(damp + nh + t);
                const __m256 bo = _mm256_broadcast_ss(omd + nh + t);
                const __m256 bf = _mm256_broadcast_ss(fb + nh + t);
                for (std::size_t ch = 0; ch < 2; ++ch) {
                    const __m256 x = _mm256_broadcast_ss(xin[ch] + nh + t);
                    fs[ch] = _mm256_add_ps(_mm256_mul_ps(v[ch][t], bo), _mm256_mul_ps(fs[ch], bd));
                    const __m256 dc_out = _mm256_add_ps(_mm256_sub_ps(fs[ch], x1[ch]),
                                                        _mm256_mul_ps(rv, y1[ch]));
                    x1[ch] = fs[ch];
                    y1[ch] = dc_out;
                    v[ch][t] = _mm256_add_ps(x, _mm256_mul_ps(bf, dc_out));
                }
            }
            for (std::size_t ch = 0; ch < 2; ++ch) {
                transpose4x4_lanes(v[ch]);
                for (std::size_t i = 0; i < 4; ++i) {
                    _mm_storeu_ps(cp[ch][i] + nh, _mm256_castps256_ps128(v[ch][i]));
                    _mm_storeu_ps(cp[ch][i + 4] + nh, _mm256_extractf128_ps(v[ch][i], 1));
                }
            }
        }
    }

    for (std::size_t ch = 0; ch < 2; ++ch) {
        _mm256_storeu_ps(st.comb_filter_state[ch], fs[ch]);
        _mm256_storeu_ps(st.dc_x1[ch], x1[ch]);
        _mm256_storeu_ps(st.dc_y1[ch], y1[ch]);
    }

    // Write the new feedback samples back into the comb lines.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t c = 0; c < NC; ++c) {
            float* buf = st.comb_buffers[ch][c];
            const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
            const std::size_t pos = st.comb_pos[ch][c];
            if (wrapped[ch][c]) {
                const std::size_t first = size - pos;
                std::memcpy(buf + pos, stage[ch][c], first * sizeof(float));
                std::memcpy(buf, stage[ch][c] + first, (N - first) * sizeof(float));
            }
            const std::size_t np = pos + N;
            st.comb_pos[ch][c] = np >= size ? np - size : np;
        }
    }

    // Series allpasses, vectorised over time.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t a = 0; a < NA; ++a) {
            float* buf = st.allpass_buffers[ch][a];
            const std::size_t size = FreeverbState::ALLPASS_SIZES_LR[ch][a];
            const std::size_t pos = st.allpass_pos[ch][a];
            const std::size_t first = std::min(N, size - pos);
            allpass_run(buf + pos, ys[ch], first);
            if (first < N) allpass_run(buf, ys[ch] + first, N - first);
            const std::size_t np = pos + N;
            st.allpass_pos[ch][a] = np >= size ? np - size : np;
        }
    }

    for (std::size_t i = 0; i < N; i += 8) {
        const __m256 xl = _mm256_loadu_ps(in_l + i);
        const __m256 xr = _mm256_loadu_ps(in_r + i);
        const __m256 dry = dry_buf ? _mm256_loadu_ps(dry_buf + i) : _mm256_set1_ps(dry_const);
        const __m256 wet = wet_buf ? _mm256_loadu_ps(wet_buf + i) : _mm256_set1_ps(wet_const);
        const __m256 ol = _mm256_add_ps(_mm256_mul_ps(xl, dry),
                                        _mm256_mul_ps(_mm256_load_ps(ys[0] + i), wet));
        const __m256 orr = _mm256_add_ps(_mm256_mul_ps(xr, dry),
                                         _mm256_mul_ps(_mm256_load_ps(ys[1] + i), wet));
        _mm256_storeu_ps(out_l + i, ol);
        _mm256_storeu_ps(out_r + i, orr);
    }
}
#endif
}  // namespace freeverb_kernels

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

// Default constants for Dattorro reverb
constexpr float DATTORRO_INPUT_DIFFUSION_DEFAULT = 0.75f;
constexpr float DATTORRO_DECAY_DIFFUSION_DEFAULT = 0.625f;
constexpr float DATTORRO_LFO_RATE_DEFAULT = 0.5f;

// ============================================================================
// REVERB_DATTORRO: Dattorro Plate Reverb
// ============================================================================
// in0: input signal
// in1: decay (0.0-0.99)
// in2: pre-delay (ms, 0-100)
// in3: input_diffusion - input smoothing (default 0.75)
// in4: decay_diffusion - tail smoothing (default 0.625)
// ext params (ExtendedParams<5>): ext[0]=damping, ext[1]=mod_depth,
//   ext[2]=lfo_rate (Hz), ext[3]=dry, ext[4]=wet
//
// High-quality plate reverb algorithm with modulation for richness.
// Uses input diffusion network + figure-8 tank topology.

[[gnu::always_inline]]
inline void op_reverb_dattorro(ExecutionContext& ctx, const Instruction& inst) {
    // Stereo-native: writes out_buffer (L) and out_buffer+1 (R) in one call.
    // When STEREO_INPUT is set in inst.flags, the primary signal input is a
    // stereo pair (inputs[0] = L, inputs[0]+1 = R). When unset, the mono input
    // is auto-broadcast to both internal lanes. See
    // prd-stereo-native-opcodes.md for the flag truth table.
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(inst.out_buffer + 1);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* decay = ctx.buffers->get(inst.inputs[1]);
    const float* predelay_ms = ctx.buffers->get(inst.inputs[2]);
    const float* input_diffusion_in = ctx.buffers->get(inst.inputs[3]);
    const float* decay_diffusion_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<DattorroState>(inst.state_id);

    // Ensure buffers are allocated from arena
    state.ensure_buffers(ctx.arena);
    if (!state.buffers_ready()) {  // arena exhausted — degrade to dry passthrough
        CEDAR_LOG_ONCE("[CEDAR] audio arena exhausted: reverb(dattorro) → dry passthrough\n");
        drywet::passthrough_stereo(input, input_r, stereo_in, out_l, out_r, BLOCK_SIZE);
        return;
    }

    // ExtendedParams<5> (prd-extended-params-migration §4.4):
    //   ext[0]=damping, ext[1]=mod_depth, ext[2]=lfo_rate (Hz),
    //   ext[3]=dry, ext[4]=wet.
    // damping/mod_depth were bit-packed in inst.rate; lfo_rate was hardcoded.
    // Fallbacks reproduce the pre-migration inst.rate=0 decode (damping=0,
    // mod_depth=0) and the DATTORRO_LFO_RATE_DEFAULT constant.
    const auto* ext = ctx.states->get_if<ExtendedParams<5>>(ext_params_state_id(inst.state_id));
    auto resolve_slot = [&](std::size_t idx, float default_val,
                            const float*& out_buf, float& out_const) {
        out_buf = nullptr;
        out_const = default_val;
        if (!ext) return;
        const auto& slot = ext->params[idx];
        if (slot.is_constant()) out_const = slot.constant;
        else                    out_buf   = ctx.buffers->get(slot.buffer_idx);
    };
    const float* damping_buf   = nullptr; float damping_const   = 0.0f;
    const float* mod_depth_buf = nullptr; float mod_depth_const = 0.0f;
    const float* lfo_rate_buf  = nullptr; float lfo_rate_const  = DATTORRO_LFO_RATE_DEFAULT;
    const float* dry_buf       = nullptr; float dry_const       = 1.0f;
    const float* wet_buf       = nullptr; float wet_const       = 0.5f;
    resolve_slot(0, 0.0f, damping_buf, damping_const);
    resolve_slot(1, 0.0f, mod_depth_buf, mod_depth_const);
    resolve_slot(2, DATTORRO_LFO_RATE_DEFAULT, lfo_rate_buf, lfo_rate_const);
    resolve_slot(3, 1.0f, dry_buf, dry_const);
    resolve_slot(4, 0.5f, wet_buf, wet_const);

    float inv_sample_rate = 1.0f / ctx.sample_rate;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float x_in_l = input[i];
        float x_in_r = stereo_in ? input_r[i] : x_in_l;
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);
        float dec = std::clamp(decay[i], 0.0f, 0.99f);
        float pre_ms = std::clamp(predelay_ms[i], 0.0f, 100.0f);

        // Runtime tunable parameters (use defaults if zero/negative)
        float input_diffusion = input_diffusion_in[i] > 0.0f ? input_diffusion_in[i] : DATTORRO_INPUT_DIFFUSION_DEFAULT;
        float decay_diffusion = decay_diffusion_in[i] > 0.0f ? decay_diffusion_in[i] : DATTORRO_DECAY_DIFFUSION_DEFAULT;
        float damping = std::clamp(damping_buf ? damping_buf[i] : damping_const, 0.0f, 1.0f);
        float mod_depth = std::clamp(mod_depth_buf ? mod_depth_buf[i] : mod_depth_const, 0.0f, 1.0f);
        // lfo_rate clamped to a positive minimum to avoid a zero phase
        // increment (prd-extended-params-migration §8 edge case).
        float lfo_rate = std::max(lfo_rate_buf ? lfo_rate_buf[i] : lfo_rate_const, 0.01f);

        // Pre-delay (per-channel duplicated path)
        float predelay_samples = pre_ms * 0.001f * ctx.sample_rate;
        predelay_samples = std::min(predelay_samples, static_cast<float>(DattorroState::PREDELAY_SIZE - 1));

        float x_l;
        float x_r;
        {
            state.predelay_buffer[0][state.predelay_pos[0]] = x_in_l;
            std::size_t read_pos = (state.predelay_pos[0] + DattorroState::PREDELAY_SIZE -
                                    static_cast<std::size_t>(predelay_samples)) % DattorroState::PREDELAY_SIZE;
            x_l = state.predelay_buffer[0][read_pos];
            state.predelay_pos[0] = (state.predelay_pos[0] + 1) % DattorroState::PREDELAY_SIZE;
        }
        {
            state.predelay_buffer[1][state.predelay_pos[1]] = x_in_r;
            std::size_t read_pos = (state.predelay_pos[1] + DattorroState::PREDELAY_SIZE -
                                    static_cast<std::size_t>(predelay_samples)) % DattorroState::PREDELAY_SIZE;
            x_r = state.predelay_buffer[1][read_pos];
            state.predelay_pos[1] = (state.predelay_pos[1] + 1) % DattorroState::PREDELAY_SIZE;
        }

        // Input diffusion: 4 allpass filters per channel
        for (std::size_t d = 0; d < DattorroState::NUM_INPUT_DIFFUSERS; ++d) {
            std::size_t size = DattorroState::INPUT_DIFFUSER_SIZES[d];

            float* buf_l = state.input_diffusers[0][d];
            float delayed_l = buf_l[state.input_pos[0][d]];
            float output_l = delayed_l - input_diffusion * x_l;
            buf_l[state.input_pos[0][d]] = x_l + input_diffusion * output_l;
            state.input_pos[0][d] = (state.input_pos[0][d] + 1) % size;
            x_l = output_l;

            float* buf_r = state.input_diffusers[1][d];
            float delayed_r = buf_r[state.input_pos[1][d]];
            float output_r = delayed_r - input_diffusion * x_r;
            buf_r[state.input_pos[1][d]] = x_r + input_diffusion * output_r;
            state.input_pos[1][d] = (state.input_pos[1][d] + 1) % size;
            x_r = output_r;
        }

        // Update modulation LFO
        state.mod_phase += lfo_rate * inv_sample_rate;
        if (state.mod_phase >= 1.0f) state.mod_phase -= 1.0f;

        // Tank processing (figure-8 topology)
        // Branch 0 ("left tank"): decay diffuser 0 -> delay 0 -> damp 0 -> DC blocker 0
        // Branch 1 ("right tank"): decay diffuser 1 -> delay 1 -> damp 1 -> DC blocker 1
        // Each branch's feedback drives the OTHER branch's input (figure-8).

        // Get feedback from opposite branch + per-channel diffused signal
        float left_in = x_l + dec * state.tank_feedback[1];
        float right_in = x_r + dec * state.tank_feedback[0];

        // Process left branch
        {
            // Decay diffuser 1
            float* buffer = state.decay_diffusers[0];
            std::size_t size = DattorroState::DECAY_DIFFUSER_SIZES[0];
            float delayed = buffer[state.decay_pos[0]];
            float output = delayed - decay_diffusion * left_in;
            buffer[state.decay_pos[0]] = left_in + decay_diffusion * output;
            state.decay_pos[0] = (state.decay_pos[0] + 1) % size;
            left_in = output;

            // Delay 1 with modulation
            float mod = std::sin(state.mod_phase * TWO_PI) * mod_depth * 8.0f;
            float delay_samples = static_cast<float>(DattorroState::DELAY_SIZES[0]) + mod;
            delay_samples = std::clamp(delay_samples, 1.0f, static_cast<float>(DattorroState::MAX_DELAY_SIZE - 1));

            left_in = delay_read_linear(state.delays[0], DattorroState::MAX_DELAY_SIZE,
                                         state.delay_pos[0], delay_samples);
            state.delays[0][state.delay_pos[0]] = output * dec;
            state.delay_pos[0] = (state.delay_pos[0] + 1) % DattorroState::MAX_DELAY_SIZE;

            // Damping filter
            state.damp_state[0] = left_in * (1.0f - damping) + state.damp_state[0] * damping;
            left_in = state.damp_state[0];

            // DC blocker in feedback path
            float dc_out_l = left_in - state.dc_x1[0] + DC_BLOCKER_R * state.dc_y1[0];
            state.dc_x1[0] = left_in;
            state.dc_y1[0] = dc_out_l;
            left_in = dc_out_l;

            state.tank_feedback[0] = left_in;
        }

        // Process right branch
        {
            // Decay diffuser 2
            float* buffer = state.decay_diffusers[1];
            std::size_t size = DattorroState::DECAY_DIFFUSER_SIZES[1];
            float delayed = buffer[state.decay_pos[1]];
            float output = delayed - decay_diffusion * right_in;
            buffer[state.decay_pos[1]] = right_in + decay_diffusion * output;
            state.decay_pos[1] = (state.decay_pos[1] + 1) % size;
            right_in = output;

            // Delay 2 with modulation (opposite phase)
            float mod = std::sin((state.mod_phase + 0.5f) * TWO_PI) * mod_depth * 8.0f;
            float delay_samples = static_cast<float>(DattorroState::DELAY_SIZES[1]) + mod;
            delay_samples = std::clamp(delay_samples, 1.0f, static_cast<float>(DattorroState::MAX_DELAY_SIZE - 1));

            right_in = delay_read_linear(state.delays[1], DattorroState::MAX_DELAY_SIZE,
                                          state.delay_pos[1], delay_samples);
            state.delays[1][state.delay_pos[1]] = output * dec;
            state.delay_pos[1] = (state.delay_pos[1] + 1) % DattorroState::MAX_DELAY_SIZE;

            // Damping filter
            state.damp_state[1] = right_in * (1.0f - damping) + state.damp_state[1] * damping;
            right_in = state.damp_state[1];

            // DC blocker in feedback path
            float dc_out_r = right_in - state.dc_x1[1] + DC_BLOCKER_R * state.dc_y1[1];
            state.dc_x1[1] = right_in;
            state.dc_y1[1] = dc_out_r;
            right_in = dc_out_r;

            state.tank_feedback[1] = right_in;
        }

        // Stereo output: L tap = branch-0 feedback, R tap = branch-1 feedback.
        // (Mono pre-PRD output was the 0.5*(L+R) average; recover with
        // (out_l + out_r) * 0.5 if needed.)
        out_l[i] = drywet::mix(x_in_l, state.tank_feedback[0], dry, wet);
        out_r[i] = drywet::mix(x_in_r, state.tank_feedback[1], dry, wet);
    }
}

// ============================================================================
// REVERB_FDN: Feedback Delay Network
// ============================================================================
// in0: input signal
// in1: decay (0.0-0.99)
// in2: damping (0.0-1.0)
// rate: room size modifier (0-255 scales delay times, 128 = 1.0x)
//
// 4x4 FDN with Hadamard mixing matrix. Provides dense, smooth reverb
// with controllable decay. Good for realistic room simulation.

[[gnu::always_inline]]
inline void op_reverb_fdn(ExecutionContext& ctx, const Instruction& inst) {
    // Stereo-native: writes out_buffer (L) and out_buffer+1 (R) in one call.
    // FDN keeps a single shared 4-line state pool; stereo is produced at the
    // output stage by emitting two diagonal Hadamard taps (delayed[0] → L,
    // delayed[1] → R). Decorrelation comes from the prime-ratio spacing
    // between those delay lines (1931 vs 2473 samples). When STEREO_INPUT is
    // set the L and R primary-input lanes are averaged into a single injection
    // signal — FDN has no per-channel topology to carry separate inputs
    // through. See prd-stereo-native-opcodes.md for the flag truth table.
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* decay = ctx.buffers->get(inst.inputs[1]);
    const float* damping = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<FDNState>(inst.state_id);

    float size_mod = 0.5f + static_cast<float>(inst.rate) / 255.0f;  // 0.5-1.5

    state.ensure_buffers(ctx.arena);
    if (!state.buffers_ready()) {  // arena exhausted — degrade to dry passthrough
        CEDAR_LOG_ONCE("[CEDAR] audio arena exhausted: reverb(fdn) → dry passthrough\n");
        drywet::passthrough_stereo(input, input_r, stereo_in, out_l, out_r, BLOCK_SIZE);
        return;
    }

    // Hadamard matrix coefficients (normalized 4x4)
    // H = 0.5 * [[1,1,1,1], [1,-1,1,-1], [1,1,-1,-1], [1,-1,-1,1]]
    constexpr float H = 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Fold stereo input to a single injection sample (preserves total
        // energy: rms of (L+R)*0.5 ≈ rms(mono)).
        float x = stereo_in ? 0.5f * (input[i] + input_r[i]) : input[i];
        float dec = std::clamp(decay[i], 0.0f, 0.99f);
        float damp = std::clamp(damping[i], 0.0f, 1.0f);
        float dry = drywet::coeff(dry_level, i, 1.0f);
        float wet = drywet::coeff(wet_level, i, 0.5f);

        // Read from all delay lines
        float delayed[FDNState::NUM_DELAYS];
        for (std::size_t d = 0; d < FDNState::NUM_DELAYS; ++d) {
            std::size_t actual_size = static_cast<std::size_t>(
                static_cast<float>(FDNState::DELAY_SIZES[d]) * size_mod);
            actual_size = std::clamp(actual_size, std::size_t{1}, FDNState::MAX_DELAY_SIZE - 1);

            std::size_t read_pos = (state.write_pos[d] + FDNState::MAX_DELAY_SIZE - actual_size)
                                   % FDNState::MAX_DELAY_SIZE;
            delayed[d] = state.delay_buffers[d][read_pos];

            // Apply damping (lowpass)
            state.damp_state[d] = delayed[d] * (1.0f - damp) + state.damp_state[d] * damp;

            // DC blocker in feedback path
            float dc_in = state.damp_state[d];
            float dc_out = dc_in - state.dc_x1[d] + DC_BLOCKER_R * state.dc_y1[d];
            state.dc_x1[d] = dc_in;
            state.dc_y1[d] = dc_out;
            delayed[d] = dc_out;
        }

        // Hadamard mixing matrix
        float mixed[FDNState::NUM_DELAYS];
        mixed[0] = H * (delayed[0] + delayed[1] + delayed[2] + delayed[3]);
        mixed[1] = H * (delayed[0] - delayed[1] + delayed[2] - delayed[3]);
        mixed[2] = H * (delayed[0] + delayed[1] - delayed[2] - delayed[3]);
        mixed[3] = H * (delayed[0] - delayed[1] - delayed[2] + delayed[3]);

        // Write to delay lines with input injection and decay
        for (std::size_t d = 0; d < FDNState::NUM_DELAYS; ++d) {
            state.delay_buffers[d][state.write_pos[d]] = x + mixed[d] * dec;
            state.write_pos[d] = (state.write_pos[d] + 1) % FDNState::MAX_DELAY_SIZE;
        }

        // Stereo output: diagonal Hadamard taps. The 0.5 factor matches the
        // historic 0.25 sum normalization in total energy when summed back to
        // mono ((L+R)*0.5 ≈ output_sum*0.25 for uncorrelated taps).
        float x_l_dry = stereo_in ? input[i] : x;
        float x_r_dry = stereo_in ? input_r[i] : x;
        out_l[i] = drywet::mix(x_l_dry, delayed[0] * 0.5f, dry, wet);
        out_r[i] = drywet::mix(x_r_dry, delayed[1] * 0.5f, dry, wet);
    }
}

}  // namespace cedar

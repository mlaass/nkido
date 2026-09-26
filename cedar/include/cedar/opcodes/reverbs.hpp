#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "dsp_utils.hpp"
#include "drywet.hpp"
#include "../util/log_once.hpp"
#include "../dsp/simd.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

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

namespace kernels {

// Copy n samples of a circular delay line starting at pos (handles wrap).
[[gnu::always_inline]]
inline void ring_read(const float* buf, std::size_t size, std::size_t pos,
                      float* dst, std::size_t n) noexcept {
    std::size_t n1 = size - pos < n ? size - pos : n;
    std::memcpy(dst, buf + pos, n1 * sizeof(float));
    std::memcpy(dst + n1, buf, (n - n1) * sizeof(float));
}

[[gnu::always_inline]]
inline void ring_write(float* buf, std::size_t size, std::size_t pos,
                       const float* src, std::size_t n) noexcept {
    std::size_t n1 = size - pos < n ? size - pos : n;
    std::memcpy(buf + pos, src, n1 * sizeof(float));
    std::memcpy(buf, src + n1, (n - n1) * sizeof(float));
}

#if CEDAR_SIMD_X86
// 8x8 register transpose.
[[gnu::always_inline]] CEDAR_TARGET_AVX2 inline void transpose8x8_avx2(__m256 (&r)[8]) noexcept {
    const __m256 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3];
    const __m256 r4 = r[4], r5 = r[5], r6 = r[6], r7 = r[7];
    __m256 t0 = _mm256_unpacklo_ps(r0, r1);
    __m256 t1 = _mm256_unpackhi_ps(r0, r1);
    __m256 t2 = _mm256_unpacklo_ps(r2, r3);
    __m256 t3 = _mm256_unpackhi_ps(r2, r3);
    __m256 t4 = _mm256_unpacklo_ps(r4, r5);
    __m256 t5 = _mm256_unpackhi_ps(r4, r5);
    __m256 t6 = _mm256_unpacklo_ps(r6, r7);
    __m256 t7 = _mm256_unpackhi_ps(r6, r7);
    __m256 s0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));
    r[0] = _mm256_permute2f128_ps(s0, s4, 0x20);
    r[1] = _mm256_permute2f128_ps(s1, s5, 0x20);
    r[2] = _mm256_permute2f128_ps(s2, s6, 0x20);
    r[3] = _mm256_permute2f128_ps(s3, s7, 0x20);
    r[4] = _mm256_permute2f128_ps(s0, s4, 0x31);
    r[5] = _mm256_permute2f128_ps(s1, s5, 0x31);
    r[6] = _mm256_permute2f128_ps(s2, s6, 0x31);
    r[7] = _mm256_permute2f128_ps(s3, s7, 0x31);
}

// Ring position of block offset i for a line of `size` (> BLOCK_SIZE).
[[gnu::always_inline]] inline std::size_t ring_at(std::size_t pos, std::size_t i,
                                                  std::size_t size) noexcept {
    std::size_t p = pos + i;
    return p >= size ? p - size : p;
}

// Every comb/allpass delay (>= 225 samples) exceeds BLOCK_SIZE, so a block's
// reads never observe its own writes: read the whole block of delayed samples
// up front, run the 8 comb recurrences of each lane as one 8-wide vector
// (same per-lane op order as scalar), then write the block back.
CEDAR_TARGET_AVX2 inline void freeverb_avx2(
    FreeverbState& state, const float* x_l, const float* x_r,
    const float* room_size, const float* damping,
    const float* room_scale_in, const float* room_offset_in,
    const float* dry_buf, float dry_const, const float* wet_buf, float wet_const,
    float* out_l, float* out_r) noexcept {
    constexpr std::size_t N = BLOCK_SIZE;
    constexpr std::size_t NC = FreeverbState::NUM_COMBS;
    static_assert(NC == 8, "freeverb_avx2 assumes 8 combs per lane");
    alignas(32) float damp_a[N];
    alignas(32) float omd_a[N];
    alignas(32) float fb_a[N];
    alignas(32) float tr[2][N][NC];    // [lane][sample][comb]
    alignas(32) float wrap_tmp[NC][8]; // chunks straddling a ring's wrap point
    alignas(32) float ysum[2][N];
    alignas(32) float apd[N];

    for (std::size_t i = 0; i < N; ++i) {
        float room = std::clamp(room_size[i], 0.0f, 1.0f);
        float damp = std::clamp(damping[i], 0.0f, 1.0f);
        float room_scale = room_scale_in[i] > 0.0f ? room_scale_in[i] : FREEVERB_ROOM_SCALE_DEFAULT;
        float room_offset = room_offset_in[i] > 0.0f ? room_offset_in[i] : FREEVERB_ROOM_OFFSET_DEFAULT;
        damp_a[i] = damp;
        omd_a[i] = 1.0f - damp;
        fb_a[i] = room * room_scale + room_offset;
    }

    // Gather delayed comb outputs straight from the rings, sum them in scalar
    // order, transpose. Only a chunk straddling a wrap point is staged.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < N; i += 8) {
            __m256 r[8];
            for (std::size_t c = 0; c < NC; ++c) {
                const float* buf = state.comb_buffers[ch][c];
                const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
                const std::size_t p = ring_at(state.comb_pos[ch][c], i, size);
                if (p + 8 <= size) [[likely]] {
                    r[c] = _mm256_loadu_ps(buf + p);
                } else {
                    for (std::size_t k = 0; k < 8; ++k)
                        wrap_tmp[c][k] = buf[ring_at(p, k, size)];
                    r[c] = _mm256_load_ps(wrap_tmp[c]);
                }
            }
            __m256 s = _mm256_setzero_ps();
            for (std::size_t c = 0; c < NC; ++c) s = _mm256_add_ps(s, r[c]);
            _mm256_store_ps(ysum[ch] + i, s);
            transpose8x8_avx2(r);
            for (std::size_t k = 0; k < 8; ++k) _mm256_store_ps(tr[ch][i + k], r[k]);
        }
    }

    // Comb recurrences: both lanes interleaved for ILP.
    const __m256 R = _mm256_set1_ps(DC_BLOCKER_R);
    __m256 fs0 = _mm256_loadu_ps(state.comb_filter_state[0]);
    __m256 fs1 = _mm256_loadu_ps(state.comb_filter_state[1]);
    __m256 x10 = _mm256_loadu_ps(state.dc_x1[0]);
    __m256 x11 = _mm256_loadu_ps(state.dc_x1[1]);
    __m256 y10 = _mm256_loadu_ps(state.dc_y1[0]);
    __m256 y11 = _mm256_loadu_ps(state.dc_y1[1]);
    for (std::size_t i = 0; i < N; ++i) {
        const __m256 dmp = _mm256_broadcast_ss(damp_a + i);
        const __m256 omd = _mm256_broadcast_ss(omd_a + i);
        const __m256 fb = _mm256_broadcast_ss(fb_a + i);
        const __m256 d0 = _mm256_load_ps(tr[0][i]);
        const __m256 d1 = _mm256_load_ps(tr[1][i]);
        fs0 = _mm256_add_ps(_mm256_mul_ps(d0, omd), _mm256_mul_ps(fs0, dmp));
        fs1 = _mm256_add_ps(_mm256_mul_ps(d1, omd), _mm256_mul_ps(fs1, dmp));
        __m256 o0 = _mm256_add_ps(_mm256_sub_ps(fs0, x10), _mm256_mul_ps(R, y10));
        __m256 o1 = _mm256_add_ps(_mm256_sub_ps(fs1, x11), _mm256_mul_ps(R, y11));
        x10 = fs0; x11 = fs1;
        y10 = o0; y11 = o1;
        _mm256_store_ps(tr[0][i], _mm256_add_ps(_mm256_broadcast_ss(x_l + i), _mm256_mul_ps(fb, o0)));
        _mm256_store_ps(tr[1][i], _mm256_add_ps(_mm256_broadcast_ss(x_r + i), _mm256_mul_ps(fb, o1)));
    }
    _mm256_storeu_ps(state.comb_filter_state[0], fs0);
    _mm256_storeu_ps(state.comb_filter_state[1], fs1);
    _mm256_storeu_ps(state.dc_x1[0], x10);
    _mm256_storeu_ps(state.dc_x1[1], x11);
    _mm256_storeu_ps(state.dc_y1[0], y10);
    _mm256_storeu_ps(state.dc_y1[1], y11);

    // Transpose back and write straight into the comb rings.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < N; i += 8) {
            __m256 r[8];
            for (std::size_t k = 0; k < 8; ++k) r[k] = _mm256_load_ps(tr[ch][i + k]);
            transpose8x8_avx2(r);
            for (std::size_t c = 0; c < NC; ++c) {
                float* buf = state.comb_buffers[ch][c];
                const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
                const std::size_t p = ring_at(state.comb_pos[ch][c], i, size);
                if (p + 8 <= size) [[likely]] {
                    _mm256_storeu_ps(buf + p, r[c]);
                } else {
                    _mm256_store_ps(wrap_tmp[c], r[c]);
                    for (std::size_t k = 0; k < 8; ++k)
                        buf[ring_at(p, k, size)] = wrap_tmp[c][k];
                }
            }
        }
        for (std::size_t c = 0; c < NC; ++c) {
            const std::size_t size = FreeverbState::COMB_SIZES_LR[ch][c];
            state.comb_pos[ch][c] = ring_at(state.comb_pos[ch][c], N, size);
        }
    }

    // Series allpasses: no intra-block recurrence, vectorise along time.
    const __m256 g = _mm256_set1_ps(0.5f);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t a = 0; a < FreeverbState::NUM_ALLPASSES; ++a) {
            float* buf = state.allpass_buffers[ch][a];
            const std::size_t size = FreeverbState::ALLPASS_SIZES_LR[ch][a];
            const std::size_t pos = state.allpass_pos[ch][a];
            ring_read(buf, size, pos, apd, N);
            for (std::size_t i = 0; i < N; i += 8) {
                __m256 y = _mm256_load_ps(ysum[ch] + i);
                __m256 out = _mm256_sub_ps(_mm256_load_ps(apd + i), _mm256_mul_ps(g, y));
                _mm256_store_ps(apd + i, _mm256_add_ps(y, _mm256_mul_ps(g, out)));
                _mm256_store_ps(ysum[ch] + i, out);
            }
            ring_write(buf, size, pos, apd, N);
            state.allpass_pos[ch][a] = (pos + N) % size;
        }
    }

    // Dry/wet mix.
    for (std::size_t i = 0; i < N; i += 8) {
        __m256 xl = _mm256_loadu_ps(x_l + i);
        __m256 xr = _mm256_loadu_ps(x_r + i);
        __m256 dry = dry_buf ? _mm256_loadu_ps(dry_buf + i) : _mm256_set1_ps(dry_const);
        __m256 wet = wet_buf ? _mm256_loadu_ps(wet_buf + i) : _mm256_set1_ps(wet_const);
        __m256 ol = _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(_mm256_load_ps(ysum[0] + i), wet));
        __m256 orr = _mm256_add_ps(_mm256_mul_ps(xr, dry), _mm256_mul_ps(_mm256_load_ps(ysum[1] + i), wet));
        _mm256_storeu_ps(out_l + i, ol);
        _mm256_storeu_ps(out_r + i, orr);
    }
}
#endif
}  // namespace kernels

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

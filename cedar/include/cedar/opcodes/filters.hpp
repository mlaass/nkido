#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "drywet.hpp"
#include "../dsp/simd.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// Clamp filter state/output to prevent blowup
// Audio signals should never exceed ±10 in normal operation
[[gnu::always_inline]]
inline float clamp_audio(float val) {
    if (val != val) return 0.0f;  // NaN check (NaN != NaN is true)
    if (val > 10.0f) return 10.0f;
    if (val < -10.0f) return -10.0f;
    return val;
}

// Tiny DC offset to prevent denormal numbers (inaudible)
constexpr float DENORMAL_DC = 1e-18f;

// SVF (State Variable Filter) coefficient calculation
inline void calc_svf(SVFState& state, float freq, float q, float sample_rate) {
    freq = std::max(0.0f, freq);

    if (freq == state.last_freq && q == state.last_q) {
        return;
    }
    state.last_freq = freq;
    state.last_q = q;

    freq = std::clamp(freq, 20.0f, sample_rate * 0.49f);
    q = std::max(0.1f, q);

    state.g = std::tan(PI * freq / sample_rate);
    state.k = 1.0f / q;
    state.a1 = 1.0f / (1.0f + state.g * (state.g + state.k));
    state.a2 = state.g * state.a1;
    state.a3 = state.g * state.a2;
}

// SVF Lowpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
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

// SVF Highpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
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

// SVF Bandpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
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

// ============================================================================
// Moog-Style Ladder Filter
// ============================================================================

// Soft saturation function (fast tanh approximation)
// Provides analog-like nonlinearity in the feedback path
[[gnu::always_inline]]
inline float soft_clip(float x) {
    // Pade approximant of tanh, accurate for |x| < 3
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Default constants for Moog ladder filter
constexpr float MOOG_MAX_RESONANCE_DEFAULT = 4.0f;
constexpr float MOOG_INPUT_SCALE_DEFAULT = 0.5f;

// FILTER_MOOG: 4-pole (24dB/oct) Moog-style ladder filter
// in0: input signal
// in1: cutoff frequency (Hz)
// in2: resonance (0.0-max_resonance, self-oscillates at ~4.0)
// in3: max_resonance - self-oscillation threshold (default 4.0)
// in4: input_scale - preamp drive/saturation (default 0.5)
//
// Based on the Huovilainen improved model for digital Moog filters
// Features nonlinear saturation in the feedback path for analog character
[[gnu::always_inline]]
inline void op_filter_moog(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* res = ctx.buffers->get(inst.inputs[2]);
    const float* max_res_in = ctx.buffers->get(inst.inputs[3]);
    const float* input_scale_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<MoogState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float cutoff = freq[i];
        float resonance = res[i];

        // Runtime tunable parameters (use defaults if zero/negative)
        float max_resonance = max_res_in[i] > 0.0f ? max_res_in[i] : MOOG_MAX_RESONANCE_DEFAULT;
        float input_scale = input_scale_in[i] > 0.0f ? input_scale_in[i] : MOOG_INPUT_SCALE_DEFAULT;
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        // Update coefficients if parameters changed (shared across channels)
        if (cutoff != state.last_freq || resonance != state.last_res) {
            state.last_freq = cutoff;
            state.last_res = resonance;

            // Frequency warping for digital implementation
            // Clamp cutoff to prevent instability at very high frequencies
            float f = std::clamp(cutoff / ctx.sample_rate, 0.0f, 0.45f);

            // Compute g coefficient using tan for frequency warping
            state.g = std::tan(PI * f);

            // Resonance coefficient (0 to max_resonance range, self-oscillates near 4)
            state.k = std::clamp(resonance, 0.0f, max_resonance);
        }

        // G = g / (1 + g) for each stage (shared)
        float G = state.g / (1.0f + state.g);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float xin = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Feedback from last stage output with nonlinear saturation
            float feedback = state.k * soft_clip(state.stage[ch][3]);

            // Input with feedback subtracted (negative feedback loop)
            float x = xin - feedback;

            // Soft clip the input to prevent harsh clipping at high input levels
            x = soft_clip(x * input_scale) * (1.0f / input_scale);

            // 4 cascaded 1-pole lowpass stages using trapezoidal integration
            for (int j = 0; j < 4; ++j) {
                float input_stage = (j == 0) ? x : state.stage[ch][j - 1];

                // Trapezoidal integration (implicit Euler)
                float v = G * (input_stage - state.delay[ch][j]);
                float y = v + state.delay[ch][j];

                // Update delay and stage output
                state.delay[ch][j] = y + v;
                state.stage[ch][j] = y;

                // Apply soft saturation between stages for analog character
                if (j < 3) {
                    state.stage[ch][j] = soft_clip(state.stage[ch][j]);
                }
            }

            // Output is the 4-pole lowpass
            float processed = clamp_audio(state.stage[ch][3]);
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(xin, processed, dry, wet);
        }
    }
}

// ============================================================================
// Diode Ladder Filter (TB-303 Acid)
// ============================================================================

// Diode nonlinearity: hyperbolic sine approximation
// True diode: I = I_s * (exp(V / V_t) - 1), but sinh provides symmetric behavior
[[gnu::always_inline]]
inline float diode_sinh(float x) {
    // Fast sinh approximation for |x| < 4
    // sinh(x) = x + x^3/6 + x^5/120 for small x
    // For larger values, use (exp(x) - exp(-x)) / 2
    if (x > 4.0f) return 27.29f + 0.5f * std::exp(x);  // exp(4)/2 ≈ 27.29
    if (x < -4.0f) return -27.29f - 0.5f * std::exp(-x);

    float x2 = x * x;
    return x * (1.0f + x2 * (0.166667f + x2 * 0.00833333f));
}

// Derivative of sinh for Newton-Raphson
[[gnu::always_inline]]
inline float diode_cosh(float x) {
    // cosh(x) = 1 + x^2/2 + x^4/24
    if (std::abs(x) > 4.0f) return 0.5f * (std::exp(std::abs(x)));

    float x2 = x * x;
    return 1.0f + x2 * (0.5f + x2 * 0.0416667f);
}

// Default constants for diode ladder filter (used when inputs provide 0 or negative values)
// VT: Thermal voltage - affects diode curve sharpness (real diode ~0.026V)
// FB_GAIN: Feedback multiplier to compensate for VT attenuation in feedback path
constexpr float DIODE_VT_DEFAULT = 0.026f;
constexpr float DIODE_FB_GAIN_DEFAULT = 10.0f;

// FILTER_DIODE: ZDF 4-pole diode ladder filter
// in0: input signal
// in1: cutoff frequency (Hz)
// in2: resonance (0.0-4.0, self-oscillates at ~3.5+)
// in3: vt - thermal voltage (affects diode curve sharpness, default 0.026)
// in4: fb_gain - feedback gain multiplier (compensates VT attenuation, default 10.0)
//
// Based on the Roland TB-303 filter topology with diode nonlinearity.
// Uses Newton-Raphson iteration for implicit integration.
// The diode nonlinearity creates the characteristic "squelchy" acid sound.
[[gnu::always_inline]]
inline void op_filter_diode(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* res = ctx.buffers->get(inst.inputs[2]);
    const float* vt_in = ctx.buffers->get(inst.inputs[3]);
    const float* fb_gain_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<DiodeState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float cutoff = freq[i];
        float resonance = res[i];

        // Runtime tunable parameters (use defaults if zero/negative)
        float vt = vt_in[i] > 0.0f ? vt_in[i] : DIODE_VT_DEFAULT;
        float vt_inv = 1.0f / vt;
        float fb_gain = fb_gain_in[i] > 0.0f ? fb_gain_in[i] : DIODE_FB_GAIN_DEFAULT;
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        // Update coefficients if parameters changed (shared across channels)
        if (cutoff != state.last_freq || resonance != state.last_res) {
            state.last_freq = cutoff;
            state.last_res = resonance;

            // Frequency warping - clamp to safe range
            float f = std::clamp(cutoff / ctx.sample_rate, 0.0f, 0.45f);

            // g coefficient using tan for frequency warping
            state.g = std::tan(PI * f);

            // Resonance: diode ladder has different feedback topology
            state.k = std::clamp(resonance, 0.0f, 4.0f);
        }

        // G for trapezoidal integration (shared)
        float G = state.g / (1.0f + state.g);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float xin = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Diode ladder feedback (per-channel cap state)
            float fb_voltage = state.cap[ch][3] * vt_inv;
            float feedback = state.k * diode_sinh(fb_voltage) * vt * fb_gain;

            // Input with feedback
            float x = xin - feedback;

            // Soft saturation at input
            x = std::tanh(x * 0.5f) * 2.0f;

            // Process 4 cascaded stages with diode nonlinearity
            for (int j = 0; j < 4; ++j) {
                float v_in = (j == 0) ? x : state.cap[ch][j - 1];

                // Diode-coupled stage: the coupling is nonlinear
                float v_est = state.cap[ch][j];
                for (int nr = 0; nr < 1; ++nr) {
                    float v_diff = v_in - v_est;

                    // Nonlinear transfer through diode
                    float diode_v = v_diff * vt_inv;
                    float i_diode = diode_sinh(diode_v);
                    float di_diode = diode_cosh(diode_v) * vt_inv;

                    // Newton-Raphson update: v_new = v_old - f(v)/f'(v)
                    float f_v = v_est - G * i_diode * vt - (1.0f - G) * state.cap[ch][j];
                    float df_v = 1.0f + G * di_diode * vt;

                    v_est = v_est - f_v / df_v;
                }

                // Clamp to prevent blowup
                state.cap[ch][j] = clamp_audio(v_est);
            }

            // Output is the 4-pole lowpass
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(xin, state.cap[ch][3], dry, wet);
        }
    }
}

// ============================================================================
// Formant (Vowel) Filter
// ============================================================================

// Vowel formant table (F1, F2, F3 in Hz)
// Based on average male voice formants
struct VowelFormants {
    float f1, f2, f3;
    float g1, g2, g3;  // Relative gains
};

constexpr VowelFormants VOWEL_TABLE[5] = {
    {650.0f, 1100.0f, 2860.0f, 1.0f, 0.5f, 0.25f},   // A (as in "father")
    {300.0f, 2300.0f, 3000.0f, 1.0f, 0.4f, 0.2f},    // I (as in "feet")
    {300.0f, 870.0f, 2240.0f, 1.0f, 0.6f, 0.3f},     // U (as in "boot")
    {400.0f, 2000.0f, 2550.0f, 1.0f, 0.45f, 0.25f},  // E (as in "bed")
    {400.0f, 800.0f, 2600.0f, 1.0f, 0.5f, 0.2f}      // O (as in "bought")
};

namespace kernels {

#if CEDAR_SIMD_X86
// Vowel-table lookup + lerp: T[ia]*(1-fr) + T[ia2]*fr.
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline __m256 formant_lerp_avx2(__m256 tab, __m256i i0, __m256i i1, __m256 omf, __m256 f) noexcept {
    return _mm256_add_ps(_mm256_mul_ps(_mm256_permutevar8x32_ps(tab, i0), omf),
                         _mm256_mul_ps(_mm256_permutevar8x32_ps(tab, i1), f));
}

// clamp_audio on 8 lanes: NaN -> 0, else clamp to [-10, 10].
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline __m256 formant_clamp_avx2(__m256 v) noexcept {
    const __m256 ord = _mm256_cmp_ps(v, v, _CMP_ORD_Q);
    __m256 r = _mm256_max_ps(v, _mm256_set1_ps(-10.0f));
    r = _mm256_min_ps(r, _mm256_set1_ps(10.0f));
    return _mm256_and_ps(r, ord);
}

// tan(w) for w in [0, 1.51] on 4 doubles: sin/cos Taylor series, one divide.
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline __m256d formant_tan_pd(__m256d w) noexcept {
    const __m256d x = _mm256_mul_pd(w, w);
    __m256d s = _mm256_set1_pd(1.0 / 6227020800.0);
    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(-1.0 / 39916800.0));
    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0 / 362880.0));
    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(-1.0 / 5040.0));
    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0 / 120.0));
    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(-1.0 / 6.0));
    s = _mm256_add_pd(_mm256_mul_pd(s, x), _mm256_set1_pd(1.0));
    s = _mm256_mul_pd(s, w);
    __m256d c = _mm256_set1_pd(-1.0 / 87178291200.0);
    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0 / 479001600.0));
    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-1.0 / 3628800.0));
    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0 / 40320.0));
    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-1.0 / 720.0));
    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0 / 24.0));
    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(-0.5));
    c = _mm256_add_pd(_mm256_mul_pd(c, x), _mm256_set1_pd(1.0));
    return _mm256_div_pd(s, c);
}

// tan(w) for w in [0, 0.25] on 4 doubles: [3/2] continued-fraction Pade,
// tan w ~ w*(105 - 10x)/(105 - 45x + x^2), x = w^2. Truncation error is
// x^4/(105*945) relative (1.5e-10 at w = 0.25, <= 0.003 float ulp).
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline __m256d formant_tan_small_pd(__m256d w) noexcept {
    const __m256d x = _mm256_mul_pd(w, w);
    const __m256d c105 = _mm256_set1_pd(105.0);
    const __m256d num = _mm256_mul_pd(w, _mm256_sub_pd(c105, _mm256_mul_pd(_mm256_set1_pd(10.0), x)));
    const __m256d den = _mm256_sub_pd(c105, _mm256_mul_pd(x, _mm256_sub_pd(_mm256_set1_pd(45.0), x)));
    return _mm256_div_pd(num, den);
}

// Prewarped SVF coefficient for 4 samples of one band: min(1.9, 2*tan(pi*min(f, 0.48*sr)/sr)).
// w is built with the same float ops as the scalar path; tan is evaluated in
// double (sin/cos Taylor, ~1e-10 rel. error for w <= 1.51) and rounded to
// float. glibc's tanf is not always correctly rounded, so lanes whose double
// result lies within 0.03 ulp of a rounding midpoint (or on a power of two)
// are re-evaluated with the real std::tan; every other lane is bit-identical
// to tanf. The filter is chaotic at high q, so 1 ulp here would blow up.
// simd: approx (replaces std::tan)
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline void formant_coef4_avx2(const float* fq, float* coef_out, float sample_rate) noexcept {
    const __m128 lim = _mm_set1_ps(sample_rate * 0.48f);
    const __m128 srv = _mm_set1_ps(sample_rate);
    const __m128 f = _mm_min_ps(_mm_load_ps(fq), lim);
    const __m128 w = _mm_div_ps(_mm_mul_ps(_mm_set1_ps(PI), f), srv);
    // Formant frequencies keep w <= 0.25 at typical sample rates: a shorter series
    // (sin to w^7, cos to w^8; rel. error <= ~5e-11 there) is enough. Larger or NaN w
    // takes the full-range series.
    const __m128 small = _mm_cmp_ps(w, _mm_set1_ps(0.25f), _CMP_NLE_UQ);
    const __m256d wd = _mm256_cvtps_pd(w);
    const bool all_small = __builtin_expect(_mm_movemask_ps(small) == 0, 1);
    const __m256d d = all_small ? formant_tan_small_pd(wd) : formant_tan_pd(wd);
    __m128 t = _mm256_cvtpd_ps(d);

    // A lane is risky if d lies within a small band of a float rounding midpoint,
    // i.e. its low 29 mantissa bits are within +-delta of 2^28. The Pade path has
    // double error <= ~0.003 ulp, so a 0.01 ulp band (0.01 * 2^29) is enough and
    // keeps ~4x fewer lanes on the slow scalar fallback; the long-series path keeps
    // the wider 0.03 ulp band. Testing d's own bits (no float round trip) also covers
    // the power-of-two boundaries, since ulp(d) is taken from d's own binade.
    const long long delta = all_small ? 5368709LL : 16106127LL;
    const __m256i off = _mm256_and_si256(
        _mm256_sub_epi64(_mm256_castpd_si256(d), _mm256_set1_epi64x((1LL << 28) - delta)),
        _mm256_set1_epi64x((1LL << 29) - 1));
    const __m256i risky = _mm256_cmpgt_epi64(_mm256_set1_epi64x(2 * delta), off);
    int mask = _mm256_movemask_pd(_mm256_castsi256_pd(risky));
    if (__builtin_expect(mask != 0, 0)) {
        alignas(16) float ta[4], wa[4];
        _mm_store_ps(ta, t);
        _mm_store_ps(wa, w);
        do {
            const int j = __builtin_ctz(mask);
            mask &= mask - 1;
            ta[j] = std::tan(wa[j]);
        } while (mask);
        t = _mm_load_ps(ta);
    }
    _mm_store_ps(coef_out, _mm_min_ps(_mm_mul_ps(_mm_set1_ps(2.0f), t), _mm_set1_ps(1.9f)));
}

// Transpose band-major coefficients of samples j..j+3 into per-sample rows
// (c1, c2, c3, 0) so a step can fetch all of them with one broadcast load.
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline void formant_pack_coef_avx2(const float (*coef)[BLOCK_SIZE], float (*cv)[4],
                                   std::size_t j) noexcept {
    const __m128 a = _mm_load_ps(coef[0] + j);
    const __m128 b = _mm_load_ps(coef[1] + j);
    const __m128 c = _mm_load_ps(coef[2] + j);
    const __m128 z = _mm_setzero_ps();
    const __m128 t0 = _mm_unpacklo_ps(a, b);
    const __m128 t1 = _mm_unpacklo_ps(c, z);
    const __m128 t2 = _mm_unpackhi_ps(a, b);
    const __m128 t3 = _mm_unpackhi_ps(c, z);
    _mm_store_ps(cv[j],     _mm_movelh_ps(t0, t1));
    _mm_store_ps(cv[j + 1], _mm_movehl_ps(t1, t0));
    _mm_store_ps(cv[j + 2], _mm_movelh_ps(t2, t3));
    _mm_store_ps(cv[j + 3], _mm_movehl_ps(t3, t2));
}

// Gain sum and dry/wet mix for samples i..i+7, from the per-sample bandpass rows
// [b1 b2 b3 pad | b1 b2 b3 pad]. Same op order as the scalar body:
// out = x*dry + ((bp1*g1 + bp2*g2) + bp3*g3)*wet.
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline void formant_post8_avx2(std::size_t i, const float (*bpbuf)[8],
                               const float (*gain)[BLOCK_SIZE],
                               const float* input, const float* input_r,
                               const float* dry_buf, float dry_const,
                               const float* wet_buf, float wet_const,
                               float* out_l, float* out_r) noexcept {
    const __m256 t0 = _mm256_unpacklo_ps(_mm256_load_ps(bpbuf[i]),     _mm256_load_ps(bpbuf[i + 1]));
    const __m256 t1 = _mm256_unpackhi_ps(_mm256_load_ps(bpbuf[i]),     _mm256_load_ps(bpbuf[i + 1]));
    const __m256 t2 = _mm256_unpacklo_ps(_mm256_load_ps(bpbuf[i + 2]), _mm256_load_ps(bpbuf[i + 3]));
    const __m256 t3 = _mm256_unpackhi_ps(_mm256_load_ps(bpbuf[i + 2]), _mm256_load_ps(bpbuf[i + 3]));
    const __m256 t4 = _mm256_unpacklo_ps(_mm256_load_ps(bpbuf[i + 4]), _mm256_load_ps(bpbuf[i + 5]));
    const __m256 t5 = _mm256_unpackhi_ps(_mm256_load_ps(bpbuf[i + 4]), _mm256_load_ps(bpbuf[i + 5]));
    const __m256 t6 = _mm256_unpacklo_ps(_mm256_load_ps(bpbuf[i + 6]), _mm256_load_ps(bpbuf[i + 7]));
    const __m256 t7 = _mm256_unpackhi_ps(_mm256_load_ps(bpbuf[i + 6]), _mm256_load_ps(bpbuf[i + 7]));
    // s_k / u_k: band k of samples 0-3 / 4-7; low lane = L, high lane = R.
    const __m256 s0 = _mm256_shuffle_ps(t0, t2, 0x44);
    const __m256 s1 = _mm256_shuffle_ps(t0, t2, 0xEE);
    const __m256 s2 = _mm256_shuffle_ps(t1, t3, 0x44);
    const __m256 u0 = _mm256_shuffle_ps(t4, t6, 0x44);
    const __m256 u1 = _mm256_shuffle_ps(t4, t6, 0xEE);
    const __m256 u2 = _mm256_shuffle_ps(t5, t7, 0x44);

    const __m256 g0 = _mm256_load_ps(gain[0] + i);
    const __m256 g1 = _mm256_load_ps(gain[1] + i);
    const __m256 g2 = _mm256_load_ps(gain[2] + i);
    const __m256 pl = _mm256_add_ps(
        _mm256_add_ps(_mm256_mul_ps(_mm256_permute2f128_ps(s0, u0, 0x20), g0),
                      _mm256_mul_ps(_mm256_permute2f128_ps(s1, u1, 0x20), g1)),
        _mm256_mul_ps(_mm256_permute2f128_ps(s2, u2, 0x20), g2));
    const __m256 pr = _mm256_add_ps(
        _mm256_add_ps(_mm256_mul_ps(_mm256_permute2f128_ps(s0, u0, 0x31), g0),
                      _mm256_mul_ps(_mm256_permute2f128_ps(s1, u1, 0x31), g1)),
        _mm256_mul_ps(_mm256_permute2f128_ps(s2, u2, 0x31), g2));

    const __m256 xl = _mm256_load_ps(input + i);
    const __m256 xr = input_r ? _mm256_load_ps(input_r + i) : xl;
    const __m256 dry = dry_buf ? _mm256_load_ps(dry_buf + i) : _mm256_set1_ps(dry_const);
    const __m256 wet = wet_buf ? _mm256_load_ps(wet_buf + i) : _mm256_set1_ps(wet_const);
    _mm256_store_ps(out_l + i, _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(pl, wet)));
    _mm256_store_ps(out_r + i, _mm256_add_ps(_mm256_mul_ps(xr, dry), _mm256_mul_ps(pr, wet)));
}

// One SVF recurrence step (sample i) for the 6 chains; stores the bandpass row.
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline void formant_step_avx2(std::size_t i, __m256& z1, __m256& z2,
                              const float (*cv)[4], const float* qinv,
                              const float* input, const float* input_r,
                              float (*bpbuf)[8]) noexcept {
    const __m256 fv = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(cv[i]));
    const __m256 qv = _mm256_broadcast_ss(qinv + i);
    __m256 xv = _mm256_broadcast_ss(input + i);
    if (input_r) xv = _mm256_blend_ps(xv, _mm256_broadcast_ss(input_r + i), 0xF0);

    const __m256 hp = _mm256_sub_ps(_mm256_sub_ps(xv, _mm256_mul_ps(z1, qv)), z2);
    const __m256 bp = _mm256_add_ps(z1, _mm256_mul_ps(fv, hp));
    const __m256 lp = _mm256_add_ps(z2, _mm256_mul_ps(fv, z1));
    // clamp_audio is the identity for |v| <= 10, so feed bp/lp straight back and
    // run the clamp only when some lane is out of range or NaN (rare, predicted
    // branch). This keeps max/min/and off the loop-carried dependency chain.
    z1 = bp;
    z2 = lp;
    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256 ten = _mm256_set1_ps(10.0f);
    const __m256 bad = _mm256_or_ps(
        _mm256_cmp_ps(_mm256_and_ps(bp, abs_mask), ten, _CMP_NLE_UQ),
        _mm256_cmp_ps(_mm256_and_ps(lp, abs_mask), ten, _CMP_NLE_UQ));
    if (__builtin_expect(_mm256_movemask_ps(bad) != 0, 0)) {
        z1 = formant_clamp_avx2(bp);
        z2 = formant_clamp_avx2(lp);
    }

    _mm256_store_ps(bpbuf[i], bp);
}

// Formant freqs, gains and 1/q for samples i..i+7 (state-independent).
[[gnu::always_inline]] CEDAR_TARGET_AVX2
inline void formant_params8_avx2(std::size_t i, const __m256* tab_f, const __m256* tab_g,
                                 const float* vowel_a, const float* vowel_b,
                                 const float* morph, const float* q_in,
                                 float (*freq)[BLOCK_SIZE], float (*gain)[BLOCK_SIZE],
                                 float* qinv) noexcept {
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256i four = _mm256_set1_epi32(4);
    const __m256i one_i = _mm256_set1_epi32(1);

    const __m256 va = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(vowel_a + i), zero), _mm256_set1_ps(4.0f));
    const __m256 vb = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(vowel_b + i), zero), _mm256_set1_ps(4.0f));
    const __m256 m = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(morph + i), zero), one);
    const __m256 q = _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(q_in + i), one), _mm256_set1_ps(20.0f));

    const __m256i ia = _mm256_min_epi32(_mm256_cvttps_epi32(va), four);
    const __m256i ib = _mm256_min_epi32(_mm256_cvttps_epi32(vb), four);
    const __m256 fa = _mm256_sub_ps(va, _mm256_cvtepi32_ps(_mm256_cvttps_epi32(va)));
    const __m256 fb = _mm256_sub_ps(vb, _mm256_cvtepi32_ps(_mm256_cvttps_epi32(vb)));
    const __m256i ia2 = _mm256_min_epi32(_mm256_add_epi32(ia, one_i), four);
    const __m256i ib2 = _mm256_min_epi32(_mm256_add_epi32(ib, one_i), four);
    const __m256 omfa = _mm256_sub_ps(one, fa);
    const __m256 omfb = _mm256_sub_ps(one, fb);
    const __m256 omm = _mm256_sub_ps(one, m);

    for (int k = 0; k < 3; ++k) {
        const __m256 f_a = formant_lerp_avx2(tab_f[k], ia, ia2, omfa, fa);
        const __m256 f_b = formant_lerp_avx2(tab_f[k], ib, ib2, omfb, fb);
        const __m256 f = _mm256_add_ps(_mm256_mul_ps(f_a, omm), _mm256_mul_ps(f_b, m));
        _mm256_store_ps(freq[k] + i, f);

        const __m256 g_a = formant_lerp_avx2(tab_g[k], ia, ia2, omfa, fa);
        const __m256 g_b = formant_lerp_avx2(tab_g[k], ib, ib2, omfb, fb);
        _mm256_store_ps(gain[k] + i, _mm256_add_ps(_mm256_mul_ps(g_a, omm), _mm256_mul_ps(g_b, m)));
    }
    _mm256_store_ps(qinv + i, _mm256_div_ps(one, q));
}

// Lane layout: [b1 b2 b3 pad | b1 b2 b3 pad], low half = L, high half = R.
CEDAR_TARGET_AVX2
inline void formant_avx2(FormantState& state, float sample_rate,
                         const float* input, const float* input_r,
                         const float* vowel_a, const float* vowel_b,
                         const float* morph, const float* q_in,
                         const float* dry_buf, float dry_const,
                         const float* wet_buf, float wet_const,
                         float* out_l, float* out_r) noexcept {
    __m256 z1 = _mm256_set_ps(0.0f, state.bp3_z1[1], state.bp2_z1[1], state.bp1_z1[1],
                              0.0f, state.bp3_z1[0], state.bp2_z1[0], state.bp1_z1[0]);
    __m256 z2 = _mm256_set_ps(0.0f, state.bp3_z2[1], state.bp2_z2[1], state.bp1_z2[1],
                              0.0f, state.bp3_z2[0], state.bp2_z2[0], state.bp1_z2[0]);

    // Pass 1+2 (state-independent, 8 samples at a time): formant targets are a pure
    // function of the clamped params, so recomputing them per sample equals the
    // scalar "recompute on change" cache. Produces formant freqs, gains, 1/q.
    alignas(32) float freq[3][BLOCK_SIZE];
    alignas(32) float gain[3][BLOCK_SIZE];
    alignas(32) float qinv[BLOCK_SIZE];

    __m256 tab_f[3], tab_g[3];
    tab_f[0] = _mm256_setr_ps(VOWEL_TABLE[0].f1, VOWEL_TABLE[1].f1, VOWEL_TABLE[2].f1, VOWEL_TABLE[3].f1, VOWEL_TABLE[4].f1, 0.0f, 0.0f, 0.0f);
    tab_f[1] = _mm256_setr_ps(VOWEL_TABLE[0].f2, VOWEL_TABLE[1].f2, VOWEL_TABLE[2].f2, VOWEL_TABLE[3].f2, VOWEL_TABLE[4].f2, 0.0f, 0.0f, 0.0f);
    tab_f[2] = _mm256_setr_ps(VOWEL_TABLE[0].f3, VOWEL_TABLE[1].f3, VOWEL_TABLE[2].f3, VOWEL_TABLE[3].f3, VOWEL_TABLE[4].f3, 0.0f, 0.0f, 0.0f);
    tab_g[0] = _mm256_setr_ps(VOWEL_TABLE[0].g1, VOWEL_TABLE[1].g1, VOWEL_TABLE[2].g1, VOWEL_TABLE[3].g1, VOWEL_TABLE[4].g1, 0.0f, 0.0f, 0.0f);
    tab_g[1] = _mm256_setr_ps(VOWEL_TABLE[0].g2, VOWEL_TABLE[1].g2, VOWEL_TABLE[2].g2, VOWEL_TABLE[3].g2, VOWEL_TABLE[4].g2, 0.0f, 0.0f, 0.0f);
    tab_g[2] = _mm256_setr_ps(VOWEL_TABLE[0].g3, VOWEL_TABLE[1].g3, VOWEL_TABLE[2].g3, VOWEL_TABLE[3].g3, VOWEL_TABLE[4].g3, 0.0f, 0.0f, 0.0f);

    // Pass 1 is software-pipelined into the recurrence loop below: the params of the
    // next 8-sample group are computed while the serial SVF chain of this one runs.
    formant_params8_avx2(0, tab_f, tab_g, vowel_a, vowel_b, morph, q_in, freq, gain, qinv);

    // Pass 2b+3 fused and software-pipelined: the SVF recurrence is a serial
    // latency-bound chain, the tan coefficients are throughput-bound and state
    // independent. Coefficients for chunk c+1 (one band per step) are issued between
    // the recurrence steps of chunk c so the out-of-order core overlaps the two.
    alignas(32) float coef[3][BLOCK_SIZE];
    constexpr std::size_t CH = 4;
    // The recurrence only stores per-sample bandpass rows; the gain sum and dry/wet
    // mix run 8 samples at a time (formant_post8_avx2) every second chunk.
    alignas(32) float cv[BLOCK_SIZE][4];
    alignas(32) float bpbuf[BLOCK_SIZE][8];
    for (int k = 0; k < 3; ++k) formant_coef4_avx2(freq[k], coef[k], sample_rate);
    formant_pack_coef_avx2(coef, cv, 0);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += CH) {
        const bool more = i + CH < BLOCK_SIZE;
        if (!(i & CH) && i + 8 < BLOCK_SIZE)
            formant_params8_avx2(i + 8, tab_f, tab_g, vowel_a, vowel_b, morph, q_in,
                                 freq, gain, qinv);
        formant_step_avx2(i, z1, z2, cv, qinv, input, input_r, bpbuf);
        if (more) formant_coef4_avx2(freq[0] + i + CH, coef[0] + i + CH, sample_rate);
        formant_step_avx2(i + 1, z1, z2, cv, qinv, input, input_r, bpbuf);
        if (more) formant_coef4_avx2(freq[1] + i + CH, coef[1] + i + CH, sample_rate);
        formant_step_avx2(i + 2, z1, z2, cv, qinv, input, input_r, bpbuf);
        if (more) {
            formant_coef4_avx2(freq[2] + i + CH, coef[2] + i + CH, sample_rate);
            formant_pack_coef_avx2(coef, cv, i + CH);
        }
        formant_step_avx2(i + 3, z1, z2, cv, qinv, input, input_r, bpbuf);
        if (i & CH)
            formant_post8_avx2(i - CH, bpbuf, gain, input, input_r,
                               dry_buf, dry_const, wet_buf, wet_const, out_l, out_r);
    }

    // Persist the scalar cache so the fallback path / next block stay consistent.
    constexpr std::size_t L = BLOCK_SIZE - 1;
    state.last_vowel_a = std::clamp(vowel_a[L], 0.0f, 4.0f);
    state.last_vowel_b = std::clamp(vowel_b[L], 0.0f, 4.0f);
    state.last_morph = std::clamp(morph[L], 0.0f, 1.0f);
    state.last_q = std::clamp(q_in[L], 1.0f, 20.0f);
    state.f1 = freq[0][L]; state.f2 = freq[1][L]; state.f3 = freq[2][L];
    state.g1 = gain[0][L]; state.g2 = gain[1][L]; state.g3 = gain[2][L];

    alignas(32) float a1[8], a2[8];
    _mm256_store_ps(a1, z1);
    _mm256_store_ps(a2, z2);
    state.bp1_z1[0] = a1[0]; state.bp2_z1[0] = a1[1]; state.bp3_z1[0] = a1[2];
    state.bp1_z1[1] = a1[4]; state.bp2_z1[1] = a1[5]; state.bp3_z1[1] = a1[6];
    state.bp1_z2[0] = a2[0]; state.bp2_z2[0] = a2[1]; state.bp3_z2[0] = a2[2];
    state.bp1_z2[1] = a2[4]; state.bp2_z2[1] = a2[5]; state.bp3_z2[1] = a2[6];
}
#endif

}  // namespace kernels

// FILTER_FORMANT: 3-band parallel vowel morphing filter
// in0: input signal
// in1: vowel_a (0-4, selects first vowel: A/I/U/E/O)
// in2: vowel_b (0-4, selects second vowel)
// in3: morph (0-1, interpolates between vowel_a and vowel_b)
// in4: q (resonance/bandwidth, 1-20)
//
// Creates vocal-like filter sweeps by morphing between vowel formants.
// Uses 3 parallel Chamberlin SVF bandpass filters.
[[gnu::always_inline]]
inline void op_filter_formant(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* vowel_a = ctx.buffers->get(inst.inputs[1]);
    const float* vowel_b = ctx.buffers->get(inst.inputs[2]);
    const float* morph = ctx.buffers->get(inst.inputs[3]);
    const float* q_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FormantState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) {
        kernels::formant_avx2(state, ctx.sample_rate, input, input_r, vowel_a, vowel_b,
                              morph, q_in, dry_buf, dry_const, wet_buf, wet_const,
                              out_l, out_r);
        return;
    }
#endif

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float va = std::clamp(vowel_a[i], 0.0f, 4.0f);
        float vb = std::clamp(vowel_b[i], 0.0f, 4.0f);
        float m = std::clamp(morph[i], 0.0f, 1.0f);
        float q = std::clamp(q_in[i], 1.0f, 20.0f);
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        // Check if we need to recalculate formant targets
        if (va != state.last_vowel_a || vb != state.last_vowel_b ||
            m != state.last_morph || q != state.last_q) {

            state.last_vowel_a = va;
            state.last_vowel_b = vb;
            state.last_morph = m;
            state.last_q = q;

            // Interpolate between vowel indices (with fractional support)
            int idx_a = static_cast<int>(va);
            int idx_b = static_cast<int>(vb);
            float frac_a = va - static_cast<float>(idx_a);
            float frac_b = vb - static_cast<float>(idx_b);

            idx_a = std::min(idx_a, 4);
            idx_b = std::min(idx_b, 4);
            int idx_a2 = std::min(idx_a + 1, 4);
            int idx_b2 = std::min(idx_b + 1, 4);

            // Get formants for vowel A (with fractional interpolation)
            float f1_a = VOWEL_TABLE[idx_a].f1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f1 * frac_a;
            float f2_a = VOWEL_TABLE[idx_a].f2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f2 * frac_a;
            float f3_a = VOWEL_TABLE[idx_a].f3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f3 * frac_a;
            float g1_a = VOWEL_TABLE[idx_a].g1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g1 * frac_a;
            float g2_a = VOWEL_TABLE[idx_a].g2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g2 * frac_a;
            float g3_a = VOWEL_TABLE[idx_a].g3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g3 * frac_a;

            // Get formants for vowel B
            float f1_b = VOWEL_TABLE[idx_b].f1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f1 * frac_b;
            float f2_b = VOWEL_TABLE[idx_b].f2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f2 * frac_b;
            float f3_b = VOWEL_TABLE[idx_b].f3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f3 * frac_b;
            float g1_b = VOWEL_TABLE[idx_b].g1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g1 * frac_b;
            float g2_b = VOWEL_TABLE[idx_b].g2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g2 * frac_b;
            float g3_b = VOWEL_TABLE[idx_b].g3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g3 * frac_b;

            // Morph between A and B
            state.f1 = f1_a * (1.0f - m) + f1_b * m;
            state.f2 = f2_a * (1.0f - m) + f2_b * m;
            state.f3 = f3_a * (1.0f - m) + f3_b * m;
            state.g1 = g1_a * (1.0f - m) + g1_b * m;
            state.g2 = g2_a * (1.0f - m) + g2_b * m;
            state.g3 = g3_a * (1.0f - m) + g3_b * m;
        }

        // Chamberlin SVF coefficients with bilinear pre-warping (shared across channels)
        auto prewarp_coef = [&](float freq) -> float {
            float w = PI * std::min(freq, ctx.sample_rate * 0.48f) / ctx.sample_rate;
            return std::min(1.9f, 2.0f * std::tan(w));
        };
        float f1_coef = prewarp_coef(state.f1);
        float f2_coef = prewarp_coef(state.f2);
        float f3_coef = prewarp_coef(state.f3);
        float q_coef = 1.0f / q;

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Bandpass 1 (F1 - first formant)
            float hp1 = x - state.bp1_z1[ch] * q_coef - state.bp1_z2[ch];
            float bp1 = state.bp1_z1[ch] + f1_coef * hp1;
            float lp1 = state.bp1_z2[ch] + f1_coef * state.bp1_z1[ch];
            state.bp1_z1[ch] = clamp_audio(bp1);
            state.bp1_z2[ch] = clamp_audio(lp1);

            // Bandpass 2 (F2 - second formant)
            float hp2 = x - state.bp2_z1[ch] * q_coef - state.bp2_z2[ch];
            float bp2 = state.bp2_z1[ch] + f2_coef * hp2;
            float lp2 = state.bp2_z2[ch] + f2_coef * state.bp2_z1[ch];
            state.bp2_z1[ch] = clamp_audio(bp2);
            state.bp2_z2[ch] = clamp_audio(lp2);

            // Bandpass 3 (F3 - third formant)
            float hp3 = x - state.bp3_z1[ch] * q_coef - state.bp3_z2[ch];
            float bp3 = state.bp3_z1[ch] + f3_coef * hp3;
            float lp3 = state.bp3_z2[ch] + f3_coef * state.bp3_z1[ch];
            state.bp3_z1[ch] = clamp_audio(bp3);
            state.bp3_z2[ch] = clamp_audio(lp3);

            // Sum bandpasses with formant gains
            float processed = bp1 * state.g1 + bp2 * state.g2 + bp3 * state.g3;
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, processed, dry, wet);
        }
    }
}

// ============================================================================
// Sallen-Key Filter (MS-20 Style)
// ============================================================================

// Default constants for Sallen-Key filter
constexpr float SALLENKEY_CLIP_THRESHOLD_DEFAULT = 0.7f;
constexpr float SALLENKEY_CLIP_POS_SLOPE_DEFAULT = 2.0f;
constexpr float SALLENKEY_CLIP_NEG_SLOPE_DEFAULT = 3.0f;

// Diode clipper function for feedback path (parameterized)
[[gnu::always_inline]]
inline float diode_clip(float x, float& state, float threshold) {
    // Asymmetric soft diode clipping
    // Positive: gentle soft knee
    // Negative: sharper knee (like silicon diode)
    // Headroom scaled with threshold to allow self-oscillation at high resonance
    constexpr float POS_SLOPE = SALLENKEY_CLIP_POS_SLOPE_DEFAULT;
    constexpr float NEG_SLOPE = SALLENKEY_CLIP_NEG_SLOPE_DEFAULT;

    float clipped;
    if (x > threshold) {
        clipped = threshold + std::tanh((x - threshold) * POS_SLOPE) * 0.8f;
    } else if (x < -threshold) {
        // Sharper negative clipping
        clipped = -threshold + std::tanh((x + threshold) * NEG_SLOPE) * 0.6f;
    } else {
        clipped = x;
    }

    // Slight hysteresis for character
    state = state * 0.1f + clipped * 0.9f;
    return state;
}

// FILTER_SALLENKEY: MS-20 style 12dB/oct filter with diode feedback
// in0: input signal
// in1: cutoff frequency (Hz)
// in2: resonance (0.0-4.0, aggressive self-oscillation)
// in3: mode (0.0 = lowpass, 1.0 = highpass)
// in4: clip_threshold - feedback clipping point (default 0.7)
//
// Based on the Korg MS-20 filter topology with diode clipping in the
// feedback path. Creates aggressive, fuzzy resonance character.
[[gnu::always_inline]]
inline void op_filter_sallenkey(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* freq = ctx.buffers->get(inst.inputs[1]);
    const float* res = ctx.buffers->get(inst.inputs[2]);
    const float* mode_in = ctx.buffers->get(inst.inputs[3]);
    const float* clip_threshold_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<SallenkeyState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float cutoff = freq[i];
        float resonance = res[i];
        float mode = mode_in[i];

        // Runtime tunable parameter (use default if zero/negative)
        float clip_threshold = clip_threshold_in[i] > 0.0f ? clip_threshold_in[i] : SALLENKEY_CLIP_THRESHOLD_DEFAULT;
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        // Update coefficients if needed (shared across channels)
        if (cutoff != state.last_freq || resonance != state.last_res) {
            state.last_freq = cutoff;
            state.last_res = resonance;

            // Frequency warping
            float f = std::clamp(cutoff / ctx.sample_rate, 0.0f, 0.45f);
            state.g = std::tan(PI * f);

            // Resonance - MS-20 has very aggressive feedback
            state.k = std::clamp(resonance, 0.0f, 4.0f);
        }

        // G for trapezoidal integration (shared)
        float G = state.g / (1.0f + state.g);
        float m = std::clamp(mode, 0.0f, 1.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float xin = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Get feedback with diode clipping (the MS-20 "scream") — per-channel state
            float fb = state.cap2[ch] * state.k;
            fb = diode_clip(fb, state.diode_state[ch], clip_threshold);

            // Input with feedback
            float x = xin - fb;

            // First stage
            float v1 = G * (x - state.cap1[ch]) + state.cap1[ch];

            // Second stage with resonance boost
            float v2 = G * (v1 - state.cap2[ch]) + state.cap2[ch];

            // Update capacitor states
            state.cap1[ch] = clamp_audio(2.0f * v1 - state.cap1[ch]);
            state.cap2[ch] = clamp_audio(2.0f * v2 - state.cap2[ch]);

            // Mode selection: 0 = lowpass, 1 = highpass
            float lp = v2;
            float hp = x - v1 * (1.0f + state.k * 0.5f) - v2;

            // Crossfade between LP and HP based on mode
            float processed = lp * (1.0f - m) + hp * m;
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(xin, processed, dry, wet);
        }
    }
}

}  // namespace cedar

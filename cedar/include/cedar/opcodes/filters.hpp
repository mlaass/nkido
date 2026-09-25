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
// Same arithmetic as the scalar formant-target update, in the same order.
CEDAR_TARGET_AVX2 inline void formant_targets_avx2(FormantState& state, float va, float vb,
                                                   float m) noexcept {
    int idx_a = static_cast<int>(va);
    int idx_b = static_cast<int>(vb);
    float frac_a = va - static_cast<float>(idx_a);
    float frac_b = vb - static_cast<float>(idx_b);

    idx_a = std::min(idx_a, 4);
    idx_b = std::min(idx_b, 4);
    int idx_a2 = std::min(idx_a + 1, 4);
    int idx_b2 = std::min(idx_b + 1, 4);

    float f1_a = VOWEL_TABLE[idx_a].f1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f1 * frac_a;
    float f2_a = VOWEL_TABLE[idx_a].f2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f2 * frac_a;
    float f3_a = VOWEL_TABLE[idx_a].f3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f3 * frac_a;
    float g1_a = VOWEL_TABLE[idx_a].g1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g1 * frac_a;
    float g2_a = VOWEL_TABLE[idx_a].g2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g2 * frac_a;
    float g3_a = VOWEL_TABLE[idx_a].g3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g3 * frac_a;

    float f1_b = VOWEL_TABLE[idx_b].f1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f1 * frac_b;
    float f2_b = VOWEL_TABLE[idx_b].f2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f2 * frac_b;
    float f3_b = VOWEL_TABLE[idx_b].f3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f3 * frac_b;
    float g1_b = VOWEL_TABLE[idx_b].g1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g1 * frac_b;
    float g2_b = VOWEL_TABLE[idx_b].g2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g2 * frac_b;
    float g3_b = VOWEL_TABLE[idx_b].g3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g3 * frac_b;

    state.f1 = f1_a * (1.0f - m) + f1_b * m;
    state.f2 = f2_a * (1.0f - m) + f2_b * m;
    state.f3 = f3_a * (1.0f - m) + f3_b * m;
    state.g1 = g1_a * (1.0f - m) + g1_b * m;
    state.g2 = g2_a * (1.0f - m) + g2_b * m;
    state.g3 = g3_a * (1.0f - m) + g3_b * m;
}

// std::clamp(v, lo, hi): v < lo ? lo : (hi < v ? hi : v); NaN passes through.
// maxps(a, b) is a > b ? a : b and minps(a, b) is a < b ? a : b, so with the
// bound as the first operand they select exactly like the two comparisons.
CEDAR_TARGET_AVX2 inline __m256 formant_clamp_avx2(__m256 v, __m256 lo, __m256 hi) noexcept {
    return _mm256_min_ps(hi, _mm256_max_ps(lo, v));
}

// VOWEL_TABLE column lerp: T[i0] * (1 - frac) + T[i1] * frac
CEDAR_TARGET_AVX2 inline __m256 formant_lerp_avx2(__m256 tbl, __m256i i0, __m256i i1,
                                                  __m256 frac, __m256 one_m_frac) noexcept {
    return _mm256_add_ps(_mm256_mul_ps(_mm256_permutevar8x32_ps(tbl, i0), one_m_frac),
                         _mm256_mul_ps(_mm256_permutevar8x32_ps(tbl, i1), frac));
}

// a * (1 - m) + b * m
CEDAR_TARGET_AVX2 inline __m256 formant_morph_avx2(__m256 a, __m256 b, __m256 m,
                                                   __m256 one_m_m) noexcept {
    return _mm256_add_ps(_mm256_mul_ps(a, one_m_m), _mm256_mul_ps(b, m));
}

// t + w * acc
CEDAR_TARGET_AVX2 inline __m256 formant_madd_avx2(float t, __m256 w, __m256 acc) noexcept {
    return _mm256_add_ps(_mm256_set1_ps(t), _mm256_mul_ps(w, acc));
}

// tanf for 2^-13 <= x < 0.6744: the fdlibm __kernel_tanf(x, 0, 1) polynomial
// (glibc's float tanf on this range), same operations in the same order.
CEDAR_TARGET_AVX2 inline __m256 formant_tan_kernel_avx2(__m256 x) noexcept {
    constexpr float T[] = {
        3.3333334327e-01f, 1.3333334029e-01f, 5.3968254477e-02f, 2.1869488060e-02f,
        8.8632395491e-03f, 3.5920790397e-03f, 1.4562094584e-03f, 5.8804126456e-04f,
        2.4646313977e-04f, 7.8179444245e-05f, 7.1407252108e-05f, -1.8558637748e-05f,
        2.5907305826e-05f,
    };
    const auto madd = formant_madd_avx2;
    const __m256 z = _mm256_mul_ps(x, x);
    const __m256 w = _mm256_mul_ps(z, z);
    // r = T1 + w*(T3 + w*(T5 + w*(T7 + w*(T9 + w*T11))))
    __m256 r = _mm256_set1_ps(T[11]);
    r = madd(T[9], w, r); r = madd(T[7], w, r); r = madd(T[5], w, r);
    r = madd(T[3], w, r); r = madd(T[1], w, r);
    // v = z*(T2 + w*(T4 + w*(T6 + w*(T8 + w*(T10 + w*T12)))))
    __m256 v = _mm256_set1_ps(T[12]);
    v = madd(T[10], w, v); v = madd(T[8], w, v); v = madd(T[6], w, v);
    v = madd(T[4], w, v); v = madd(T[2], w, v);
    v = _mm256_mul_ps(z, v);
    const __m256 s = _mm256_mul_ps(z, x);
    // y == 0: r = z*(s*(r+v)); r += T0*s; return x + r
    r = _mm256_mul_ps(z, _mm256_mul_ps(s, _mm256_add_ps(r, v)));
    r = _mm256_add_ps(r, _mm256_mul_ps(_mm256_set1_ps(T[0]), s));
    return _mm256_add_ps(x, r);
}

// Out of line so the hot loop contains no call (ymm registers are all
// caller-saved; a call inside the loop pushes the compiler into spilling).
[[gnu::noinline, gnu::cold]] inline void formant_tan_lanes_cold(float* lanes) noexcept {
    for (int k = 0; k < 8; ++k) lanes[k] = std::tan(lanes[k]);
}

// min(1.9, 2 * tan(PI * min(f, sr * 0.48) / sr)); std::tan per lane when any
// argument leaves the range the polynomial kernel covers.
CEDAR_TARGET_AVX2 inline __m256 formant_prewarp_avx2(__m256 f, __m256 lim, __m256 sr) noexcept {
    __m256 w = _mm256_div_ps(_mm256_mul_ps(_mm256_set1_ps(PI), _mm256_min_ps(f, lim)), sr);
    const __m256 lo = _mm256_set1_ps(0x1p-13f);
    const __m256 hi = _mm256_castsi256_ps(_mm256_set1_epi32(0x3f2ca140));  // 0.6744
    const __m256 ok = _mm256_and_ps(_mm256_cmp_ps(w, lo, _CMP_GE_OQ),
                                    _mm256_cmp_ps(w, hi, _CMP_LT_OQ));
    __m256 t;
    if (__builtin_expect(_mm256_movemask_ps(ok) == 0xFF, 1)) {
        t = formant_tan_kernel_avx2(w);
    } else {
        alignas(32) float lanes[8];
        _mm256_store_ps(lanes, w);
        formant_tan_lanes_cold(lanes);
        t = _mm256_load_ps(lanes);
    }
    return _mm256_min_ps(_mm256_mul_ps(_mm256_set1_ps(2.0f), t), _mm256_set1_ps(1.9f));
}

// Store [a b c 0] per sample for 8 samples: dst[(i + k) * 4 + j].
CEDAR_TARGET_AVX2 inline void formant_store3_avx2(float* dst, __m256 a, __m256 b,
                                                  __m256 c) noexcept {
    const __m256 zero = _mm256_setzero_ps();
    __m256 t0 = _mm256_unpacklo_ps(a, b), t1 = _mm256_unpacklo_ps(c, zero);
    __m256 t2 = _mm256_unpackhi_ps(a, b), t3 = _mm256_unpackhi_ps(c, zero);
    __m256 r0 = _mm256_shuffle_ps(t0, t1, 0x44);  // samples 0 | 4
    __m256 r1 = _mm256_shuffle_ps(t0, t1, 0xEE);  // samples 1 | 5
    __m256 r2 = _mm256_shuffle_ps(t2, t3, 0x44);  // samples 2 | 6
    __m256 r3 = _mm256_shuffle_ps(t2, t3, 0xEE);  // samples 3 | 7
    _mm256_store_ps(dst + 0, _mm256_permute2f128_ps(r0, r1, 0x20));
    _mm256_store_ps(dst + 8, _mm256_permute2f128_ps(r2, r3, 0x20));
    _mm256_store_ps(dst + 16, _mm256_permute2f128_ps(r0, r1, 0x31));
    _mm256_store_ps(dst + 24, _mm256_permute2f128_ps(r2, r3, 0x31));
}

// clamp_audio on the bit pattern with 1-cycle integer ops (the float
// max/min/cmp version sits on the recurrence's critical path): non-negative
// float magnitudes order like their int bits, so |v| is clamped with
// min_epi32, the sign is put back, and NaN magnitudes (> inf bits) become +0.
CEDAR_TARGET_AVX2 inline __m256 formant_clamp_audio_avx2(__m256 v) noexcept {
    const __m256i vi = _mm256_castps_si256(v);
    const __m256i mag = _mm256_and_si256(vi, _mm256_set1_epi32(0x7fffffff));
    const __m256i sign = _mm256_and_si256(vi, _mm256_set1_epi32(static_cast<int>(0x80000000u)));
    const __m256i nan = _mm256_cmpgt_epi32(mag, _mm256_set1_epi32(0x7f800000));
    const __m256i c = _mm256_or_si256(_mm256_min_epi32(mag, _mm256_set1_epi32(0x41200000)),
                                      sign);  // 0x41200000 = 10.0f
    return _mm256_castsi256_ps(_mm256_andnot_si256(nan, c));
}

// The SVF input of sample i as [L L L L | R R R R]: Mono broadcasts it from
// the input itself, otherwise xs holds the prepared vectors.
template <bool Mono>
CEDAR_TARGET_AVX2 inline __m256 formant_x_avx2(const float* xs, std::size_t i) noexcept {
    if constexpr (Mono) return _mm256_broadcast_ss(xs + i);
    else return _mm256_load_ps(xs + i * 8);
}

// SVF recurrence over samples [i0, i0 + 8); the raw bp vector of each sample
// goes to bps for the gain sum.
template <bool Mono>
[[gnu::noinline, gnu::cold]] CEDAR_TARGET_AVX2 inline void
formant_svf_clamped_avx2(std::size_t i0, __m256& z1r, __m256& z2r, const float* coefs,
                         const float* qcs, const float* xs, float* bps) noexcept {
    __m256 z1 = z1r, z2 = z2r;

    for (std::size_t i = i0; i < i0 + 8; ++i) {
        const __m256 coef = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(coefs + i * 4));
        const __m256 qc = _mm256_broadcast_ss(qcs + i);
        const __m256 x = formant_x_avx2<Mono>(xs, i);

        __m256 hp = _mm256_sub_ps(_mm256_sub_ps(x, _mm256_mul_ps(z1, qc)), z2);
        __m256 bp = _mm256_add_ps(z1, _mm256_mul_ps(coef, hp));
        __m256 lp = _mm256_add_ps(z2, _mm256_mul_ps(coef, z1));
        z1 = formant_clamp_audio_avx2(bp);
        z2 = formant_clamp_audio_avx2(lp);
        _mm256_store_ps(bps + i * 8, bp);
    }
    z1r = z1;
    z2r = z2;
}

// clamp_audio for non-NaN v in two 1-cycle ops on the bit pattern: the signed
// min clamps positives to 10.0f (negatives are negative ints, untouched), the
// unsigned min clamps negatives to -10.0f (positives are smaller unsigned).
// A NaN comes out as +-10 here instead of +0.
CEDAR_TARGET_AVX2 inline __m256 formant_clamp_audio_nonan_avx2(__m256 v) noexcept {
    const __m256i c = _mm256_min_epi32(_mm256_castps_si256(v), _mm256_set1_epi32(0x41200000));
    return _mm256_castsi256_ps(
        _mm256_min_epu32(c, _mm256_set1_epi32(static_cast<int>(0xC1200000u))));
}

// The recurrence with the NaN-less clamp on the critical path; an unordered
// compare of bp/lp is accumulated alongside, and if any was a NaN the chunk
// is redone with the exact clamp.
template <bool Mono>
CEDAR_TARGET_AVX2 inline void formant_svf_avx2(std::size_t i0, __m256& z1r, __m256& z2r,
                                               const float* coefs, const float* qcs,
                                               const float* xs, float* bps) noexcept {
    __m256 z1 = z1r, z2 = z2r;
    __m256 nan = _mm256_setzero_ps();

    for (std::size_t i = i0; i < i0 + 8; ++i) {
        const __m256 coef = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(coefs + i * 4));
        const __m256 qc = _mm256_broadcast_ss(qcs + i);
        const __m256 x = formant_x_avx2<Mono>(xs, i);

        __m256 hp = _mm256_sub_ps(_mm256_sub_ps(x, _mm256_mul_ps(z1, qc)), z2);
        __m256 bp = _mm256_add_ps(z1, _mm256_mul_ps(coef, hp));
        __m256 lp = _mm256_add_ps(z2, _mm256_mul_ps(coef, z1));
        nan = _mm256_or_ps(nan, _mm256_cmp_ps(bp, lp, _CMP_UNORD_Q));
        z1 = formant_clamp_audio_nonan_avx2(bp);
        z2 = formant_clamp_audio_nonan_avx2(lp);
        _mm256_store_ps(bps + i * 8, bp);
    }
    if (__builtin_expect(_mm256_testz_ps(nan, nan), 1)) {
        z1r = z1;
        z2r = z2;
    } else {
        formant_svf_clamped_avx2<Mono>(i0, z1r, z2r, coefs, qcs, xs, bps);
    }
}

// (b1*g1 + b2*g2) + b3*g3
CEDAR_TARGET_AVX2 inline __m256 formant_gsum_avx2(__m256 b1, __m256 b2, __m256 b3, __m256 g1,
                                                  __m256 g2, __m256 g3) noexcept {
    return _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(b1, g1), _mm256_mul_ps(b2, g2)),
                         _mm256_mul_ps(b3, g3));
}

// Formant gain sum (bp1*g1 + bp2*g2) + bp3*g3, then drywet::mix, over
// samples [i, i + 8). bps rows are transposed to per-band columns first.
CEDAR_TARGET_AVX2 inline void formant_mix_avx2(std::size_t i, const float* in_l,
                                               const float* in_r, const float* bps,
                                               const float* g1s, const float* g2s,
                                               const float* g3s, const float* dry_buf,
                                               float dry_const, const float* wet_buf,
                                               float wet_const, float* out_l,
                                               float* out_r) noexcept {
    const float* b = bps + i * 8;
    __m256 t0 = _mm256_unpacklo_ps(_mm256_load_ps(b + 0), _mm256_load_ps(b + 8));
    __m256 t1 = _mm256_unpackhi_ps(_mm256_load_ps(b + 0), _mm256_load_ps(b + 8));
    __m256 t2 = _mm256_unpacklo_ps(_mm256_load_ps(b + 16), _mm256_load_ps(b + 24));
    __m256 t3 = _mm256_unpackhi_ps(_mm256_load_ps(b + 16), _mm256_load_ps(b + 24));
    __m256 t4 = _mm256_unpacklo_ps(_mm256_load_ps(b + 32), _mm256_load_ps(b + 40));
    __m256 t5 = _mm256_unpackhi_ps(_mm256_load_ps(b + 32), _mm256_load_ps(b + 40));
    __m256 t6 = _mm256_unpacklo_ps(_mm256_load_ps(b + 48), _mm256_load_ps(b + 56));
    __m256 t7 = _mm256_unpackhi_ps(_mm256_load_ps(b + 48), _mm256_load_ps(b + 56));
    // u*: samples 0-3 of bands (1 | 1R), (2 | 2R), (3 | 3R); v*: samples 4-7
    __m256 u0 = _mm256_shuffle_ps(t0, t2, 0x44), v0 = _mm256_shuffle_ps(t4, t6, 0x44);
    __m256 u1 = _mm256_shuffle_ps(t0, t2, 0xEE), v1 = _mm256_shuffle_ps(t4, t6, 0xEE);
    __m256 u2 = _mm256_shuffle_ps(t1, t3, 0x44), v2 = _mm256_shuffle_ps(t5, t7, 0x44);
    const __m256 g1 = _mm256_load_ps(g1s + i);
    const __m256 g2 = _mm256_load_ps(g2s + i);
    const __m256 g3 = _mm256_load_ps(g3s + i);
    const __m256 proc_l = formant_gsum_avx2(_mm256_permute2f128_ps(u0, v0, 0x20),
                                            _mm256_permute2f128_ps(u1, v1, 0x20),
                                            _mm256_permute2f128_ps(u2, v2, 0x20), g1, g2, g3);
    const __m256 proc_r = formant_gsum_avx2(_mm256_permute2f128_ps(u0, v0, 0x31),
                                            _mm256_permute2f128_ps(u1, v1, 0x31),
                                            _mm256_permute2f128_ps(u2, v2, 0x31), g1, g2, g3);

    __m256 dry = dry_buf ? _mm256_loadu_ps(dry_buf + i) : _mm256_set1_ps(dry_const);
    __m256 wet = wet_buf ? _mm256_loadu_ps(wet_buf + i) : _mm256_set1_ps(wet_const);
    __m256 xl = _mm256_loadu_ps(in_l + i);
    __m256 xr = _mm256_loadu_ps(in_r + i);
    __m256 ol = _mm256_add_ps(_mm256_mul_ps(xl, dry), _mm256_mul_ps(proc_l, wet));
    __m256 orr = _mm256_add_ps(_mm256_mul_ps(xr, dry), _mm256_mul_ps(proc_r, wet));
    _mm256_storeu_ps(out_l + i, ol);
    _mm256_storeu_ps(out_r + i, orr);
}

// Lanes: [F1 F2 F3 - | F1 F2 F3 -], low half = left, high half = right.
// The formant targets are a pure function of the clamped (va, vb, m), so the
// scalar change-detection cache is equivalent to recomputing them per sample;
// that is done 8 samples at a time, one chunk ahead of the SVF recurrence.
// Mono: in_l == in_r.
template <bool Mono>
CEDAR_TARGET_AVX2 inline void formant_avx2_impl(const float* in_l, const float* in_r,
                                                const float* vowel_a, const float* vowel_b,
                                                const float* morph, const float* q_in,
                                                const float* dry_buf, float dry_const,
                                                const float* wet_buf, float wet_const,
                                                float sr, FormantState& state,
                                                float* out_l, float* out_r) noexcept {
    __m256 z1 = _mm256_setr_ps(state.bp1_z1[0], state.bp2_z1[0], state.bp3_z1[0], 0.0f,
                               state.bp1_z1[1], state.bp2_z1[1], state.bp3_z1[1], 0.0f);
    __m256 z2 = _mm256_setr_ps(state.bp1_z2[0], state.bp2_z2[0], state.bp3_z2[0], 0.0f,
                               state.bp1_z2[1], state.bp2_z2[1], state.bp3_z2[1], 0.0f);
    alignas(32) float coefs[BLOCK_SIZE * 4];
    alignas(32) float g1s[BLOCK_SIZE], g2s[BLOCK_SIZE], g3s[BLOCK_SIZE];
    alignas(32) float qcs[BLOCK_SIZE];
    alignas(32) float xs[BLOCK_SIZE * 8];
    alignas(32) float bps[BLOCK_SIZE * 8];

    const VowelFormants* T = VOWEL_TABLE;
    const __m256 tf1 = _mm256_setr_ps(T[0].f1, T[1].f1, T[2].f1, T[3].f1, T[4].f1, 0, 0, 0);
    const __m256 tf2 = _mm256_setr_ps(T[0].f2, T[1].f2, T[2].f2, T[3].f2, T[4].f2, 0, 0, 0);
    const __m256 tf3 = _mm256_setr_ps(T[0].f3, T[1].f3, T[2].f3, T[3].f3, T[4].f3, 0, 0, 0);
    const __m256 tg1 = _mm256_setr_ps(T[0].g1, T[1].g1, T[2].g1, T[3].g1, T[4].g1, 0, 0, 0);
    const __m256 tg2 = _mm256_setr_ps(T[0].g2, T[1].g2, T[2].g2, T[3].g2, T[4].g2, 0, 0, 0);
    const __m256 tg3 = _mm256_setr_ps(T[0].g3, T[1].g3, T[2].g3, T[3].g3, T[4].g3, 0, 0, 0);
    const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
    const __m256 four = _mm256_set1_ps(4.0f), twenty = _mm256_set1_ps(20.0f);
    const __m256i ifour = _mm256_set1_epi32(4), ione = _mm256_set1_epi32(1);
    const __m256 srv = _mm256_set1_ps(sr), lim = _mm256_set1_ps(sr * 0.48f);

    // Leave the change-detection cache as the scalar path would.
    {
        float va = std::clamp(vowel_a[BLOCK_SIZE - 1], 0.0f, 4.0f);
        float vb = std::clamp(vowel_b[BLOCK_SIZE - 1], 0.0f, 4.0f);
        float m = std::clamp(morph[BLOCK_SIZE - 1], 0.0f, 1.0f);
        float q = std::clamp(q_in[BLOCK_SIZE - 1], 1.0f, 20.0f);
        if (va != state.last_vowel_a || vb != state.last_vowel_b ||
            m != state.last_morph || q != state.last_q) {
            state.last_vowel_a = va;
            state.last_vowel_b = vb;
            state.last_morph = m;
            state.last_q = q;
            formant_targets_avx2(state, va, vb, m);
        }
    }

    // Skewed loop: coefficients for samples [i, i + 8), then the SVF over
    // [i - 8, i). The recurrence is latency-bound; the independent
    // coefficient work issued alongside it fills those stalls, as does the
    // gain sum / dry-wet mix of [i - 16, i - 8), whose bp values are done.
    for (std::size_t i = 0; i <= BLOCK_SIZE + 8; i += 8) {
        if (i < BLOCK_SIZE) {
            __m256 va = formant_clamp_avx2(_mm256_loadu_ps(vowel_a + i), zero, four);
            __m256 vb = formant_clamp_avx2(_mm256_loadu_ps(vowel_b + i), zero, four);
            __m256 m = formant_clamp_avx2(_mm256_loadu_ps(morph + i), zero, one);
            __m256 q = formant_clamp_avx2(_mm256_loadu_ps(q_in + i), one, twenty);

            __m256i ia = _mm256_cvttps_epi32(va);
            __m256i ib = _mm256_cvttps_epi32(vb);
            __m256 fa = _mm256_sub_ps(va, _mm256_cvtepi32_ps(ia));
            __m256 fb = _mm256_sub_ps(vb, _mm256_cvtepi32_ps(ib));
            __m256 nfa = _mm256_sub_ps(one, fa), nfb = _mm256_sub_ps(one, fb);
            __m256 nm = _mm256_sub_ps(one, m);
            ia = _mm256_min_epi32(ia, ifour);
            ib = _mm256_min_epi32(ib, ifour);
            __m256i ia2 = _mm256_min_epi32(_mm256_add_epi32(ia, ione), ifour);
            __m256i ib2 = _mm256_min_epi32(_mm256_add_epi32(ib, ione), ifour);

#define CEDAR_FORMANT_TARGET(tbl)                                          \
    formant_morph_avx2(formant_lerp_avx2(tbl, ia, ia2, fa, nfa),           \
                       formant_lerp_avx2(tbl, ib, ib2, fb, nfb), m, nm)
            __m256 f1 = CEDAR_FORMANT_TARGET(tf1);
            __m256 f2 = CEDAR_FORMANT_TARGET(tf2);
            __m256 f3 = CEDAR_FORMANT_TARGET(tf3);
            __m256 g1 = CEDAR_FORMANT_TARGET(tg1);
            __m256 g2 = CEDAR_FORMANT_TARGET(tg2);
            __m256 g3 = CEDAR_FORMANT_TARGET(tg3);
#undef CEDAR_FORMANT_TARGET

            formant_store3_avx2(coefs + i * 4, formant_prewarp_avx2(f1, lim, srv),
                                formant_prewarp_avx2(f2, lim, srv),
                                formant_prewarp_avx2(f3, lim, srv));
            _mm256_store_ps(g1s + i, g1);
            _mm256_store_ps(g2s + i, g2);
            _mm256_store_ps(g3s + i, g3);
            _mm256_store_ps(qcs + i, _mm256_div_ps(one, q));

            // xs: [L L L L | R R R R] per sample
            if constexpr (!Mono) {
                const __m256 xl = _mm256_loadu_ps(in_l + i), xr = _mm256_loadu_ps(in_r + i);
                const __m256 lo = _mm256_permute2f128_ps(xl, xr, 0x20);
                const __m256 hi = _mm256_permute2f128_ps(xl, xr, 0x31);
                float* xd = xs + i * 8;
                _mm256_store_ps(xd + 0, _mm256_permute_ps(lo, 0x00));
                _mm256_store_ps(xd + 8, _mm256_permute_ps(lo, 0x55));
                _mm256_store_ps(xd + 16, _mm256_permute_ps(lo, 0xAA));
                _mm256_store_ps(xd + 24, _mm256_permute_ps(lo, 0xFF));
                _mm256_store_ps(xd + 32, _mm256_permute_ps(hi, 0x00));
                _mm256_store_ps(xd + 40, _mm256_permute_ps(hi, 0x55));
                _mm256_store_ps(xd + 48, _mm256_permute_ps(hi, 0xAA));
                _mm256_store_ps(xd + 56, _mm256_permute_ps(hi, 0xFF));
            }
        }
        if (i >= 16)
            formant_mix_avx2(i - 16, in_l, in_r, bps, g1s, g2s, g3s, dry_buf, dry_const,
                             wet_buf, wet_const, out_l, out_r);
        if (i != 0 && i <= BLOCK_SIZE)
            formant_svf_avx2<Mono>(i - 8, z1, z2, coefs, qcs, Mono ? in_l : xs, bps);
    }

    alignas(32) float zz1[8], zz2[8];
    _mm256_store_ps(zz1, z1);
    _mm256_store_ps(zz2, z2);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        state.bp1_z1[ch] = zz1[ch * 4 + 0]; state.bp1_z2[ch] = zz2[ch * 4 + 0];
        state.bp2_z1[ch] = zz1[ch * 4 + 1]; state.bp2_z2[ch] = zz2[ch * 4 + 1];
        state.bp3_z1[ch] = zz1[ch * 4 + 2]; state.bp3_z2[ch] = zz2[ch * 4 + 2];
    }
}

CEDAR_TARGET_AVX2 inline void formant_avx2(const float* in_l, const float* in_r,
                                           const float* vowel_a, const float* vowel_b,
                                           const float* morph, const float* q_in,
                                           const float* dry_buf, float dry_const,
                                           const float* wet_buf, float wet_const,
                                           float sr, FormantState& state,
                                           float* out_l, float* out_r) noexcept {
    if (in_l == in_r)
        formant_avx2_impl<true>(in_l, in_r, vowel_a, vowel_b, morph, q_in, dry_buf, dry_const,
                                wet_buf, wet_const, sr, state, out_l, out_r);
    else
        formant_avx2_impl<false>(in_l, in_r, vowel_a, vowel_b, morph, q_in, dry_buf, dry_const,
                                 wet_buf, wet_const, sr, state, out_l, out_r);
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
    {
        const float* in_r = stereo_in ? input_r : input;
        // The kernel reads the input again after the filter pass, so an
        // output aliasing an input takes the scalar path.
        const bool alias = out_l == input || out_l == in_r || out_r == input || out_r == in_r;
        if (!alias && simd::has(simd::Isa::Avx2)) {
            kernels::formant_avx2(input, in_r, vowel_a, vowel_b, morph, q_in,
                                  dry_buf, dry_const, wet_buf, wet_const,
                                  ctx.sample_rate, state, out_l, out_r);
            return;
        }
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

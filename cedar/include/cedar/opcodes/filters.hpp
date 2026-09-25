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

namespace kernels {
#if CEDAR_SIMD_X86
CEDAR_TARGET_AVX2 inline __m256 svf_splat(float v) noexcept { return _mm256_set1_ps(v); }

// tanf for float x in (2^-13, 3pi/4): a lane-wise transcription of the
// fdlibm/glibc float algorithm (rem_pio2f for n = 1, then __kernel_tanf),
// so results track std::tan's own rounding.
// simd: approx — replaces std::tan in calc_svf.
CEDAR_TARGET_AVX2 inline __m256 svf_tanf_ps(__m256 xin) noexcept {
    const __m256i ixin = _mm256_castps_si256(xin);
    // |x| > pi/4 (0x3f490fda): x - pi/2 with 24+24 bit pi/2, iy = -1.
    const __m256 caseb = _mm256_castsi256_ps(_mm256_cmpgt_epi32(ixin, _mm256_set1_epi32(0x3f490fda)));
    const __m256 zr = _mm256_sub_ps(xin, _mm256_set1_ps(1.5707855225e+00f));
    const __m256 pio2_1t = _mm256_set1_ps(1.0804334124e-05f);
    const __m256 y0 = _mm256_sub_ps(zr, pio2_1t);
    const __m256 y1 = _mm256_sub_ps(_mm256_sub_ps(zr, y0), pio2_1t);
    const __m256 x = _mm256_blendv_ps(xin, y0, caseb);
    const __m256 y = _mm256_and_ps(y1, caseb);
    const __m256 iy = _mm256_blendv_ps(_mm256_set1_ps(1.0f), _mm256_set1_ps(-1.0f), caseb);

    const __m256 signmask = _mm256_set1_ps(-0.0f);
    const __m256 neg = _mm256_castsi256_ps(_mm256_srai_epi32(_mm256_castps_si256(x), 31));
    const __m256i ix = _mm256_castps_si256(_mm256_andnot_ps(signmask, x));
    const __m256 big = _mm256_castsi256_ps(_mm256_cmpgt_epi32(ix, _mm256_set1_epi32(0x3f2ca13f)));
    // |x| >= 0.6744: x = (pio4 - |x|) + (pio4lo - sign*y)
    const __m256 xa = _mm256_andnot_ps(signmask, x);
    const __m256 ya = _mm256_xor_ps(y, _mm256_and_ps(neg, signmask));
    const __m256 xb = _mm256_add_ps(_mm256_sub_ps(_mm256_set1_ps(7.8539812565e-01f), xa),
                                    _mm256_sub_ps(_mm256_set1_ps(3.7748947079e-08f), ya));
    const __m256 X = _mm256_blendv_ps(x, xb, big);
    const __m256 Y = _mm256_andnot_ps(big, y);

    const __m256 z = _mm256_mul_ps(X, X);
    const __m256 w = _mm256_mul_ps(z, z);
    constexpr auto c = svf_splat;
    __m256 r = _mm256_add_ps(c(7.8179444245e-05f), _mm256_mul_ps(w, c(-1.8558637748e-05f)));
    r = _mm256_add_ps(c(5.8804126456e-04f), _mm256_mul_ps(w, r));
    r = _mm256_add_ps(c(3.5920790397e-03f), _mm256_mul_ps(w, r));
    r = _mm256_add_ps(c(2.1869488060e-02f), _mm256_mul_ps(w, r));
    r = _mm256_add_ps(c(1.3333334029e-01f), _mm256_mul_ps(w, r));
    __m256 v = _mm256_add_ps(c(7.1407252108e-05f), _mm256_mul_ps(w, c(2.5907305826e-05f)));
    v = _mm256_add_ps(c(2.4646313977e-04f), _mm256_mul_ps(w, v));
    v = _mm256_add_ps(c(1.4562094584e-03f), _mm256_mul_ps(w, v));
    v = _mm256_add_ps(c(8.8632395491e-03f), _mm256_mul_ps(w, v));
    v = _mm256_add_ps(c(5.3968254477e-02f), _mm256_mul_ps(w, v));
    v = _mm256_mul_ps(z, v);
    const __m256 s = _mm256_mul_ps(z, X);
    r = _mm256_add_ps(Y, _mm256_mul_ps(z, _mm256_add_ps(_mm256_mul_ps(s, _mm256_add_ps(r, v)), Y)));
    r = _mm256_add_ps(r, _mm256_mul_ps(c(3.3333334327e-01f), s));
    const __m256 W = _mm256_add_ps(X, r);

    // big: sgn * (iy - 2*(X - (W*W/(W+iy) - r)))
    const __m256 sgn = _mm256_blendv_ps(c(1.0f), c(-1.0f), neg);
    __m256 rb = _mm256_sub_ps(_mm256_div_ps(_mm256_mul_ps(W, W), _mm256_add_ps(W, iy)), r);
    rb = _mm256_mul_ps(sgn, _mm256_sub_ps(iy, _mm256_mul_ps(c(2.0f), _mm256_sub_ps(X, rb))));
    // big and |X| < 2^-13: sgn*iy * (1 - 2*iy*X)
    const __m256 tiny = _mm256_cmp_ps(_mm256_andnot_ps(signmask, xb), c(0x1p-13f), _CMP_LT_OQ);
    const __m256 rt = _mm256_mul_ps(_mm256_mul_ps(sgn, iy),
                                    _mm256_sub_ps(c(1.0f), _mm256_mul_ps(_mm256_mul_ps(c(2.0f), iy), xb)));
    rb = _mm256_blendv_ps(rb, rt, tiny);
    // small, iy = -1: -1/(X+r) computed accurately
    const __m256 hmask = _mm256_castsi256_ps(_mm256_set1_epi32(static_cast<int>(0xfffff000u)));
    const __m256 zt = _mm256_and_ps(W, hmask);
    const __m256 vt = _mm256_sub_ps(r, _mm256_sub_ps(zt, X));
    const __m256 a = _mm256_div_ps(c(-1.0f), W);
    const __m256 t = _mm256_and_ps(a, hmask);
    const __m256 st = _mm256_add_ps(c(1.0f), _mm256_mul_ps(t, zt));
    const __m256 rm = _mm256_add_ps(t, _mm256_mul_ps(a, _mm256_add_ps(st, _mm256_mul_ps(t, vt))));
    const __m256 rs = _mm256_blendv_ps(W, rm, caseb);
    return _mm256_blendv_ps(rs, rb, big);
}

CEDAR_TARGET_AVX2 inline __m128 svf_clamp_ps(__m128 v) noexcept {
    const __m128 c = _mm_min_ps(_mm_max_ps(v, _mm_set1_ps(-10.0f)), _mm_set1_ps(10.0f));
    return _mm_and_ps(c, _mm_cmpord_ps(v, v));  // NaN -> 0
}

// Mode: 0 = lowpass (v2), 1 = highpass, 2 = bandpass (v1).
// Coefficients are a pure function of (freq, q), so they are computed for the
// whole block first; only the integrator recurrence stays serial.
template <int Mode>
CEDAR_TARGET_AVX2 inline void svf_avx2(SVFState& state, float* out_l, float* out_r,
                                       const float* in_l, const float* in_r,
                                       const float* freq, const float* q,
                                       const float* dry_level, const float* wet_level,
                                       float sample_rate) noexcept {
    alignas(32) float A1[BLOCK_SIZE], A2[BLOCK_SIZE], A3[BLOCK_SIZE], K[BLOCK_SIZE], G[BLOCK_SIZE];
    alignas(32) float F0[BLOCK_SIZE];
    alignas(32) float Y[2 * BLOCK_SIZE];

    const __m256 hi = _mm256_set1_ps(sample_rate * 0.49f);
    const __m256 lo = _mm256_set1_ps(20.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 qmin = _mm256_set1_ps(0.1f);
    const __m256 pi = _mm256_set1_ps(PI);
    const __m256 sr = _mm256_set1_ps(sample_rate);
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8) {
        const __m256 f = _mm256_load_ps(freq + i);
        const __m256 f0 = _mm256_and_ps(_mm256_cmp_ps(zero, f, _CMP_LT_OQ), f);  // max(0, f)
        __m256 fc = _mm256_blendv_ps(f0, hi, _mm256_cmp_ps(hi, f0, _CMP_LT_OQ));
        fc = _mm256_blendv_ps(fc, lo, _mm256_cmp_ps(f0, lo, _CMP_LT_OQ));
        const __m256 qv = _mm256_load_ps(q + i);
        const __m256 qc = _mm256_blendv_ps(qmin, qv, _mm256_cmp_ps(qmin, qv, _CMP_LT_OQ));
        const __m256 x = _mm256_div_ps(_mm256_mul_ps(pi, fc), sr);
        const __m256 g = svf_tanf_ps(x);
        const __m256 k = _mm256_div_ps(one, qc);
        const __m256 a1 = _mm256_div_ps(one, _mm256_add_ps(one, _mm256_mul_ps(g, _mm256_add_ps(g, k))));
        const __m256 a2 = _mm256_mul_ps(g, a1);
        const __m256 a3 = _mm256_mul_ps(g, a2);
        _mm256_store_ps(F0 + i, f0);
        _mm256_store_ps(G + i, g);
        _mm256_store_ps(K + i, k);
        _mm256_store_ps(A1 + i, a1);
        _mm256_store_ps(A2 + i, a2);
        _mm256_store_ps(A3 + i, a3);
    }
    // Honour the scalar coefficient cache for a leading run of unchanged params.
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (!(F0[i] == state.last_freq && q[i] == state.last_q)) break;
        G[i] = state.g; K[i] = state.k; A1[i] = state.a1; A2[i] = state.a2; A3[i] = state.a3;
    }
    constexpr std::size_t L = BLOCK_SIZE - 1;
    state.last_freq = F0[L];
    state.last_q = q[L];
    state.g = G[L]; state.k = K[L]; state.a1 = A1[L]; state.a2 = A2[L]; state.a3 = A3[L];

    __m128 s1 = _mm_setr_ps(state.ic1eq[0], state.ic1eq[1], 0.0f, 0.0f);
    __m128 s2 = _mm_setr_ps(state.ic2eq[0], state.ic2eq[1], 0.0f, 0.0f);
    const __m128 dc = _mm_set1_ps(DENORMAL_DC);
    const __m128 two = _mm_set1_ps(2.0f);
    const __m128 lim = _mm_set1_ps(10.0f);
    const __m128 absmask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        const __m128 x = in_r ? _mm_unpacklo_ps(_mm_load_ss(in_l + i), _mm_load_ss(in_r + i))
                              : _mm_set1_ps(in_l[i]);
        const __m128 a1 = _mm_set1_ps(A1[i]);
        const __m128 a2 = _mm_set1_ps(A2[i]);
        const __m128 a3 = _mm_set1_ps(A3[i]);
        const __m128 t1 = _mm_add_ps(s1, dc);
        const __m128 t2 = _mm_add_ps(s2, dc);
        const __m128 v3 = _mm_sub_ps(x, t2);
        const __m128 v1 = _mm_add_ps(_mm_mul_ps(a1, t1), _mm_mul_ps(a2, v3));
        const __m128 v2 = _mm_add_ps(_mm_add_ps(t2, _mm_mul_ps(a2, t1)), _mm_mul_ps(a3, v3));
        __m128 n1 = _mm_sub_ps(_mm_mul_ps(two, v1), s1);
        __m128 n2 = _mm_sub_ps(_mm_mul_ps(two, v2), s2);
        const __m128 bad = _mm_or_ps(_mm_cmp_ps(_mm_and_ps(n1, absmask), lim, _CMP_NLE_UQ),
                                     _mm_cmp_ps(_mm_and_ps(n2, absmask), lim, _CMP_NLE_UQ));
        if (__builtin_expect((_mm_movemask_ps(bad) & 3) != 0, 0)) {
            n1 = svf_clamp_ps(n1);
            n2 = svf_clamp_ps(n2);
        }
        s1 = n1;
        s2 = n2;
        __m128 y;
        if constexpr (Mode == 0) y = v2;
        else if constexpr (Mode == 2) y = v1;
        else y = _mm_sub_ps(_mm_sub_ps(x, _mm_mul_ps(_mm_set1_ps(K[i]), v1)), v2);
        _mm_storel_pi(reinterpret_cast<__m64*>(Y + 2 * i), y);
    }
    alignas(16) float st1[4], st2[4];
    _mm_store_ps(st1, s1);
    _mm_store_ps(st2, s2);
    state.ic1eq[0] = st1[0]; state.ic1eq[1] = st1[1];
    state.ic2eq[0] = st2[0]; state.ic2eq[1] = st2[1];

    for (std::size_t i = 0; i < BLOCK_SIZE; i += 4) {
        const __m128 a = _mm_load_ps(Y + 2 * i);
        const __m128 b = _mm_load_ps(Y + 2 * i + 4);
        const __m128 yl = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
        const __m128 yr = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
        const __m128 xl = _mm_load_ps(in_l + i);
        const __m128 xr = in_r ? _mm_load_ps(in_r + i) : xl;
        const __m128 dry = dry_level ? _mm_load_ps(dry_level + i) : _mm_setzero_ps();
        const __m128 wet = wet_level ? _mm_load_ps(wet_level + i) : _mm_set1_ps(1.0f);
        _mm_store_ps(out_l + i, _mm_add_ps(_mm_mul_ps(xl, dry), _mm_mul_ps(yl, wet)));
        _mm_store_ps(out_r + i, _mm_add_ps(_mm_mul_ps(xr, dry), _mm_mul_ps(yr, wet)));
    }
}
#endif
}  // namespace kernels

// SVF Lowpass — stereo-native (prd-stereo-native-opcodes Phase 4a)
[[gnu::always_inline]]
inline void op_filter_svf_lp(ExecutionContext& ctx, const Instruction& inst) {
    [[maybe_unused]] constexpr int SVF_MODE = 0;
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
#if CEDAR_SIMD_X86
    if constexpr (BLOCK_SIZE % 8 == 0) {
        if (simd::has(simd::Isa::Avx2)) {
            kernels::svf_avx2<SVF_MODE>(state, out_l, out_r, input, input_r, freq, q,
                                        dry_level, wet_level, ctx.sample_rate);
            return;
        }
    }
#endif

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
    [[maybe_unused]] constexpr int SVF_MODE = 1;
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
#if CEDAR_SIMD_X86
    if constexpr (BLOCK_SIZE % 8 == 0) {
        if (simd::has(simd::Isa::Avx2)) {
            kernels::svf_avx2<SVF_MODE>(state, out_l, out_r, input, input_r, freq, q,
                                        dry_level, wet_level, ctx.sample_rate);
            return;
        }
    }
#endif

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
    [[maybe_unused]] constexpr int SVF_MODE = 2;
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
#if CEDAR_SIMD_X86
    if constexpr (BLOCK_SIZE % 8 == 0) {
        if (simd::has(simd::Isa::Avx2)) {
            kernels::svf_avx2<SVF_MODE>(state, out_l, out_r, input, input_r, freq, q,
                                        dry_level, wet_level, ctx.sample_rate);
            return;
        }
    }
#endif

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

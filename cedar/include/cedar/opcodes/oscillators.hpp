#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#ifndef CEDAR_NO_MINBLEP
#include "minblep.hpp"
#endif
#ifndef CEDAR_NO_FFT
#include "../wavetable/bank.hpp"
#endif
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// PolyBLEP Anti-Aliasing Functions
// ============================================================================
// PolyBLEP (Polynomial Band-Limited Step) reduces aliasing by applying
// polynomial correction near waveform discontinuities.

// PolyBLEP residual function
// t: current phase (0 to 1)
// dt: phase increment (normalized frequency)
// Returns correction value to subtract from naive waveform
[[gnu::always_inline]]
inline float poly_blep(float t, float dt) {
    // dt should be positive and less than 0.5
    dt = std::abs(dt);
    if (dt < 1e-8f) return 0.0f;  // Avoid division by zero

    if (t < dt) {
        // Just after discontinuity (phase near 0)
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        // Just before discontinuity (phase near 1)
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// Symmetric PolyBLEP using signed distance to discontinuity
// This ensures identical treatment of rising and falling edges
// distance: signed distance to discontinuity (negative = before, positive = after)
// dt: phase increment (normalized frequency)
[[gnu::always_inline]]
inline float poly_blep_distance(float distance, float dt) {
    if (dt < 1e-8f) return 0.0f;

    if (distance >= 0.0f && distance < dt) {
        // Just after discontinuity
        float t = distance / dt;  // [0, 1)
        return t + t - t * t - 1.0f;
    } else if (distance < 0.0f && distance > -dt) {
        // Just before discontinuity
        float t = distance / dt;  // (-1, 0]
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// PolyBLAMP (Polynomial Band-Limited Ramp) for triangle waves
// Integrated version of PolyBLEP for ramp discontinuities (slope changes)
[[gnu::always_inline]]
inline float poly_blamp(float t, float dt) {
    dt = std::abs(dt);
    if (dt < 1e-8f) return 0.0f;

    if (t < dt) {
        t = t / dt - 1.0f;
        return -1.0f / 3.0f * t * t * t;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt + 1.0f;
        return 1.0f / 3.0f * t * t * t;
    }
    return 0.0f;
}

// ============================================================================
// Phase Reset Helper for Trigger-based Phase Control
// ============================================================================

// Get buffer pointer, falling back to BUFFER_ZERO for unused inputs
[[gnu::always_inline]]
inline const float* get_input_or_zero(ExecutionContext& ctx, std::uint16_t buffer_id) {
    if (buffer_id == BUFFER_UNUSED) {
        return ctx.buffers->get(BUFFER_ZERO);
    }
    return ctx.buffers->get(buffer_id);
}

// Check for rising edge trigger and reset phase to offset if triggered
// Returns true if phase was reset (useful for MinBLEP buffer clearing)
[[gnu::always_inline]]
inline bool check_phase_reset(float& phase, float& prev_trigger, bool& initialized,
                               float trigger, float phase_offset) {
    // Detect rising edge (previous <= 0, current > 0)
    bool triggered = (trigger > 0.0f && prev_trigger <= 0.0f);
    prev_trigger = trigger;

    if (triggered) {
        // Normalize phase offset to [0, 1)
        phase = std::fmod(phase_offset, 1.0f);
        if (phase < 0.0f) phase += 1.0f;
        // Reset initialized to avoid PolyBLEP artifacts at reset point
        initialized = false;
        return true;
    }
    return false;
}

// SIN oscillator: out = sin(phase * 2pi), frequency from in0
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Note: Sine has no discontinuities so no anti-aliasing needed
[[gnu::always_inline]]
inline void op_osc_sin(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        out[i] = std::sin(state.phase * TWO_PI);

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

// TRI oscillator: triangle wave with PolyBLAMP anti-aliasing
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: -1 to +1, linear rise then fall
[[gnu::always_inline]]
inline void op_osc_tri(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive triangle: 4 * |phase - 0.5| - 1
        // At phase 0: 4*0.5-1 = 1 (peak)
        // At phase 0.25: 4*0.25-1 = 0
        // At phase 0.5: 4*0-1 = -1 (trough)
        // At phase 0.75: 4*0.25-1 = 0
        float value = 4.0f * std::abs(state.phase - 0.5f) - 1.0f;

        // Apply PolyBLAMP correction at slope discontinuities (skip on first sample)
        if (state.initialized) {
            // Corner at phase = 0 (slope changes from -4 to +4)
            float blamp = poly_blamp(state.phase, dt);

            // Corner at phase = 0.5 (slope changes from +4 to -4)
            float phase_half = state.phase + 0.5f;
            if (phase_half >= 1.0f) phase_half -= 1.0f;
            blamp -= poly_blamp(phase_half, dt);

            // Scale by 4*dt (slope magnitude * phase increment)
            // The factor of 4 comes from the triangle slope magnitude
            value += 4.0f * dt * blamp;
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// SAW oscillator: sawtooth wave with PolyBLEP anti-aliasing
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: -1 to +1, linear ramp up then instant reset
[[gnu::always_inline]]
inline void op_osc_saw(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive sawtooth: 2 * phase - 1
        float value = 2.0f * state.phase - 1.0f;

        // Apply PolyBLEP correction at the falling edge (skip on first sample)
        if (state.initialized) {
            value -= poly_blep(state.phase, dt);
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// SQR oscillator: square wave with PolyBLEP anti-aliasing
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: +1 for first half of cycle, -1 for second half
[[gnu::always_inline]]
inline void op_osc_sqr(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive square: +1 if phase < 0.5, else -1
        float value = (state.phase < 0.5f) ? 1.0f : -1.0f;

        // Apply PolyBLEP correction at both edges (skip on first sample)
        if (state.initialized) {
            // Rising edge at phase = 0 (transition from -1 to +1)
            value += poly_blep(state.phase, dt);

            // Falling edge at phase = 0.5 (transition from +1 to -1)
            float t = state.phase + 0.5f;
            if (t >= 1.0f) t -= 1.0f;
            value -= poly_blep(t, dt);
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// RAMP oscillator: inverted sawtooth (descending ramp) with PolyBLEP
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Output: +1 to -1, linear ramp down then instant reset
[[gnu::always_inline]]
inline void op_osc_ramp(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive ramp (inverted saw): 1 - 2 * phase
        float value = 1.0f - 2.0f * state.phase;

        // Apply PolyBLEP correction at the rising edge (skip on first sample)
        if (state.initialized) {
            value += poly_blep(state.phase, dt);
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// PHASOR: raw phase output (0 to 1)
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Useful as modulation source or for custom waveshaping
// Note: Discontinuity at phase wrap is intentional for phasor use
[[gnu::always_inline]]
inline void op_osc_phasor(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        out[i] = state.phase;

        state.prev_phase = state.phase;
        state.phase += freq[i] * ctx.inv_sample_rate;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
    }
}

#ifndef CEDAR_NO_MINBLEP
// ============================================================================
// MinBLEP Oscillators - Perfect harmonic purity for PWM and distortion
// ============================================================================

// SQR_MINBLEP oscillator: square wave with MinBLEP anti-aliasing
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Perfect harmonic purity - no even harmonics, ideal for PWM and distortion
[[gnu::always_inline]]
inline void op_osc_sqr_minblep(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<MinBLEPOscState>(inst.state_id);

    const auto& minblep_table = get_minblep_table();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        if (trigger[i] > 0.0f && state.prev_trigger <= 0.0f) {
            // Reset phase to offset
            state.phase = std::fmod(phase_offset[i], 1.0f);
            if (state.phase < 0.0f) state.phase += 1.0f;
            state.initialized = false;
            // Clear MinBLEP buffer on reset to avoid artifacts
            state.reset();
        }
        state.prev_trigger = trigger[i];

        float dt = freq[i] * ctx.inv_sample_rate;

        // Naive square wave based on current phase (PRE-advance value)
        float naive_value = (state.phase < 0.5f) ? 1.0f : -1.0f;

        // Calculate next phase
        float next_phase = state.phase + dt;

        // Check for discontinuities and add residual corrections
        if (state.initialized) {
            // Rising edge at phase = 0 (wrapping from 1.0 to 0.0)
            // Step from -1 to +1, amplitude = 2
            if (next_phase >= 1.0f) {
                state.add_step(2.0f, 0.0f, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = 1.0f;  // Switch to post-transition value
            }

            // Falling edge at phase = 0.5
            // Step from +1 to -1, amplitude = -2
            if (state.phase < 0.5f && next_phase >= 0.5f) {
                state.add_step(-2.0f, 0.0f, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = -1.0f;  // Switch to post-transition value
            }
        }

        // Output = naive + residual correction
        out[i] = naive_value + state.get_and_advance();

        // Advance phase
        state.phase = next_phase;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        }

        state.initialized = true;
    }
}
#endif // CEDAR_NO_MINBLEP

// ============================================================================
// PWM Oscillators - Pulse Width Modulation
// ============================================================================

// SQR_PWM oscillator: square wave with variable pulse width
// in0: frequency (Hz)
// in1: PWM (-1 to +1, where 0 = 50% duty cycle)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
// PWM mapping: duty = 0.5 + pwm * 0.5, so [-1,+1] maps to [0,1]
[[gnu::always_inline]]
inline void op_osc_sqr_pwm(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Calculate duty cycle from PWM input
        float duty = 0.5f + std::clamp(pwm[i], -1.0f, 1.0f) * 0.5f;
        duty = std::clamp(duty, 0.001f, 0.999f);  // Prevent degenerate cases

        // Naive square: +1 if phase < duty, else -1
        float value = (state.phase < duty) ? 1.0f : -1.0f;

        if (state.initialized) {
            // Rising edge at phase = 0 (fixed position)
            value += poly_blep(state.phase, dt);

            // Falling edge at phase = duty (variable position)
            // Calculate signed distance to falling edge
            float dist_to_fall = state.phase - duty;
            if (dist_to_fall > 0.5f) dist_to_fall -= 1.0f;
            if (dist_to_fall < -0.5f) dist_to_fall += 1.0f;
            value -= poly_blep_distance(dist_to_fall, dt);
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

// SAW_PWM oscillator: variable-slope sawtooth (morphs saw to tri to ramp)
// in0: frequency (Hz)
// in1: PWM (-1 to +1)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
// PWM = -1: Rising ramp (standard saw)
// PWM = 0: Triangle
// PWM = +1: Falling ramp (inverted saw)
[[gnu::always_inline]]
inline void op_osc_saw_pwm(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState>(inst.state_id);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.phase, state.prev_trigger, state.initialized,
                          trigger[i], phase_offset[i]);

        float dt = freq[i] * ctx.inv_sample_rate;

        // Map PWM to midpoint position: [-1,+1] -> [0.05, 0.95]
        float mid = (1.0f + std::clamp(pwm[i], -1.0f, 1.0f)) * 0.5f;
        mid = std::clamp(mid, 0.05f, 0.95f);

        // Piecewise linear waveform:
        // Phase [0, mid]: rise from -1 to +1
        // Phase [mid, 1]: fall from +1 to -1
        float value;
        if (state.phase < mid) {
            value = 2.0f * state.phase / mid - 1.0f;
        } else {
            value = 1.0f - 2.0f * (state.phase - mid) / (1.0f - mid);
        }

        if (state.initialized) {
            // PolyBLAMP at slope discontinuities
            // Corner at phase = 0 (slope change from falling to rising)
            float blamp = poly_blamp(state.phase, dt);

            // Corner at phase = mid (slope change from rising to falling)
            float phase_at_mid = state.phase - mid;
            if (phase_at_mid < 0.0f) phase_at_mid += 1.0f;
            blamp -= poly_blamp(phase_at_mid, dt);

            // Scale by slope difference
            float slope_rise = 2.0f / mid;
            float slope_fall = -2.0f / (1.0f - mid);
            float slope_diff = slope_rise - slope_fall;  // Total slope change at corners

            value += slope_diff * dt * blamp;
        }

        out[i] = value;

        // Advance phase
        state.prev_phase = state.phase;
        state.phase += dt;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        } else if (state.phase < 0.0f) {
            state.phase += 1.0f;
        }
        state.initialized = true;
    }
}

#ifndef CEDAR_NO_MINBLEP
// SQR_PWM_MINBLEP oscillator: highest quality PWM square wave
// in0: frequency (Hz)
// in1: PWM (-1 to +1, where 0 = 50% duty cycle)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
// Uses MinBLEP for sub-sample accurate edge placement
[[gnu::always_inline]]
inline void op_osc_sqr_pwm_minblep(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<MinBLEPOscState>(inst.state_id);

    const auto& minblep_table = get_minblep_table();

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        if (trigger[i] > 0.0f && state.prev_trigger <= 0.0f) {
            // Reset phase to offset
            state.phase = std::fmod(phase_offset[i], 1.0f);
            if (state.phase < 0.0f) state.phase += 1.0f;
            state.initialized = false;
            // Clear MinBLEP buffer on reset to avoid artifacts
            state.reset();
        }
        state.prev_trigger = trigger[i];

        float dt = freq[i] * ctx.inv_sample_rate;

        // Calculate duty cycle from PWM
        float duty = 0.5f + std::clamp(pwm[i], -1.0f, 1.0f) * 0.5f;
        duty = std::clamp(duty, 0.001f, 0.999f);

        float naive_value = (state.phase < duty) ? 1.0f : -1.0f;
        float next_phase = state.phase + dt;

        if (state.initialized) {
            // Rising edge at phase wrap (0 crossing)
            if (next_phase >= 1.0f) {
                float frac = (dt > 1e-8f) ? (next_phase - 1.0f) / dt : 0.0f;
                state.add_step(2.0f, frac, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = 1.0f;
            }

            // Falling edge at duty crossing
            if (state.phase < duty && next_phase >= duty) {
                float frac = (dt > 1e-8f) ? (next_phase - duty) / dt : 0.0f;
                state.add_step(-2.0f, frac, minblep_table.data(), MINBLEP_PHASES, MINBLEP_SAMPLES);
                naive_value = -1.0f;
            }
        }

        out[i] = naive_value + state.get_and_advance();

        state.phase = next_phase;
        if (state.phase >= 1.0f) {
            state.phase -= 1.0f;
        }
        state.initialized = true;
    }
}
#endif // CEDAR_NO_MINBLEP

// ============================================================================
// Oversampled Oscillators - For alias-free FM synthesis
// ============================================================================

// SIN_4X: 4x oversampled sine oscillator
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_sin_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        // Interpolate frequency between this sample and next
        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        // Generate 4 samples at 4x rate with interpolated frequency
        for (int j = 0; j < 4; ++j) {
            // Linear interpolation across the 4 sub-samples
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            samples[j] = std::sin(state.osc.phase * TWO_PI);
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// SAW_4X: 4x oversampled sawtooth
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_saw_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = 2.0f * state.osc.phase - 1.0f;

            if (state.osc.initialized) {
                value -= poly_blep(state.osc.phase, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// SQR_4X: 4x oversampled square
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_sqr_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = (state.osc.phase < 0.5f) ? 1.0f : -1.0f;

            if (state.osc.initialized) {
                value += poly_blep(state.osc.phase, dt);
                float phase_half = state.osc.phase + 0.5f;
                if (phase_half >= 1.0f) phase_half -= 1.0f;
                value -= poly_blep(phase_half, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// TRI_4X: 4x oversampled triangle
// in0: frequency (Hz)
// in1: phase offset (0-1, optional)
// in2: trigger (reset phase on rising edge, optional)
// Interpolates frequency input for true sample-accurate FM
[[gnu::always_inline]]
inline void op_osc_tri_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = 4.0f * std::abs(state.osc.phase - 0.5f) - 1.0f;

            if (state.osc.initialized) {
                float blamp = poly_blamp(state.osc.phase, dt);
                float phase_half = state.osc.phase + 0.5f;
                if (phase_half >= 1.0f) phase_half -= 1.0f;
                blamp -= poly_blamp(phase_half, dt);
                value += 4.0f * dt * blamp;
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// ============================================================================
// 4x Oversampled PWM Oscillators - For alias-free FM synthesis with PWM
// ============================================================================

// SQR_PWM_4X: 4x oversampled PWM square wave
// in0: frequency (Hz)
// in1: PWM (-1 to +1, where 0 = 50% duty cycle)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
[[gnu::always_inline]]
inline void op_osc_sqr_pwm_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];
        float pwm_val = pwm[i];

        // Calculate duty cycle from PWM input
        float duty = 0.5f + std::clamp(pwm_val, -1.0f, 1.0f) * 0.5f;
        duty = std::clamp(duty, 0.001f, 0.999f);

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            float value = (state.osc.phase < duty) ? 1.0f : -1.0f;

            if (state.osc.initialized) {
                // Rising edge at phase = 0
                value += poly_blep(state.osc.phase, dt);

                // Falling edge at phase = duty
                float dist_to_fall = state.osc.phase - duty;
                if (dist_to_fall > 0.5f) dist_to_fall -= 1.0f;
                if (dist_to_fall < -0.5f) dist_to_fall += 1.0f;
                value -= poly_blep_distance(dist_to_fall, dt);
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

// SAW_PWM_4X: 4x oversampled variable-slope sawtooth
// in0: frequency (Hz)
// in1: PWM (-1 to +1)
// in2: phase offset (0-1, optional)
// in3: trigger (reset phase on rising edge, optional)
[[gnu::always_inline]]
inline void op_osc_saw_pwm_4x(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq = ctx.buffers->get(inst.inputs[0]);
    const float* pwm = ctx.buffers->get(inst.inputs[1]);
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[2]);
    const float* trigger = get_input_or_zero(ctx, inst.inputs[3]);
    auto& state = ctx.states->get_or_create<OscState4x>(inst.state_id);

    float inv_sr_4x = ctx.inv_sample_rate * 0.25f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Check for phase reset trigger
        check_phase_reset(state.osc.phase, state.osc.prev_trigger, state.osc.initialized,
                          trigger[i], phase_offset[i]);

        float freq_curr = freq[i];
        float freq_next = (i + 1 < BLOCK_SIZE) ? freq[i + 1] : freq[i];
        float pwm_val = pwm[i];

        // Map PWM to midpoint position
        float mid = (1.0f + std::clamp(pwm_val, -1.0f, 1.0f)) * 0.5f;
        mid = std::clamp(mid, 0.05f, 0.95f);

        float samples[4];

        for (int j = 0; j < 4; ++j) {
            float t = static_cast<float>(j) * 0.25f;
            float freq_interp = freq_curr + t * (freq_next - freq_curr);
            float dt = freq_interp * inv_sr_4x;

            // Piecewise linear waveform
            float value;
            if (state.osc.phase < mid) {
                value = 2.0f * state.osc.phase / mid - 1.0f;
            } else {
                value = 1.0f - 2.0f * (state.osc.phase - mid) / (1.0f - mid);
            }

            if (state.osc.initialized) {
                // PolyBLAMP at slope discontinuities
                float blamp = poly_blamp(state.osc.phase, dt);
                float phase_at_mid = state.osc.phase - mid;
                if (phase_at_mid < 0.0f) phase_at_mid += 1.0f;
                blamp -= poly_blamp(phase_at_mid, dt);

                float slope_rise = 2.0f / mid;
                float slope_fall = -2.0f / (1.0f - mid);
                float slope_diff = slope_rise - slope_fall;

                value += slope_diff * dt * blamp;
            }

            samples[j] = value;

            state.osc.prev_phase = state.osc.phase;
            state.osc.phase += dt;
            if (state.osc.phase >= 1.0f) state.osc.phase -= 1.0f;
            else if (state.osc.phase < 0.0f) state.osc.phase += 1.0f;
            state.osc.initialized = true;
        }

        out[i] = state.downsample(samples[0], samples[1], samples[2], samples[3]);
    }
}

#ifndef CEDAR_NO_FFT
// ============================================================================
// Wavetable Oscillator (Smooch)
// ============================================================================

// 4-point Niemitalo-optimal interpolation kernel ("Optimal 2x (4-point,
// 3rd-order) z-form"). Coefficients tuned for minimal aliasing on
// oversampled material — see Olli Niemitalo, "Polynomial Interpolators for
// High-Quality Resampling of Oversampled Audio" (deip.pdf, 2001). Same
// arithmetic shape and per-sample cost as 4-point Hermite/Lagrange.
[[gnu::always_inline]]
inline float niemitalo4(float x, float y0, float y1, float y2, float y3) {
    const float z = x - 0.5f;
    const float even1 = y2 + y1, odd1 = y2 - y1;
    const float even2 = y3 + y0, odd2 = y3 - y0;
    const float c0 = even1 *  0.45645918406487612f + even2 *  0.04354173901996461f;
    const float c1 = odd1  *  0.47236675362442071f + odd2  *  0.17686613581136501f;
    const float c2 = even1 * -0.25367479420455852f + even2 *  0.25371918651882464f;
    const float c3 = odd1  * -0.37917091811631082f + odd2  *  0.11952965755786543f;
    return ((c3 * z + c2) * z + c1) * z + c0;
}

// Single (frame, mip) read with Niemitalo-4 + bitmask wrap.
[[gnu::always_inline]]
inline float wt_sample_at(const WavetableFrame& frame,
                          std::size_t mip_idx,
                          int int_pos,
                          float frac_pos) {
    constexpr int MASK = WAVETABLE_SIZE - 1;
    const auto& tbl = frame.mipMaps[mip_idx];
    const float y0 = tbl[(int_pos - 1) & MASK];
    const float y1 = tbl[ int_pos      & MASK];
    const float y2 = tbl[(int_pos + 1) & MASK];
    const float y3 = tbl[(int_pos + 2) & MASK];
    return niemitalo4(frac_pos, y0, y1, y2, y3);
}

// OSC_WAVETABLE: mip-mapped wavetable oscillator with 4-point Niemitalo
// interpolation, equal-power crossfade across mip-pyramid octave boundaries,
// and equal-power morph between phase-aligned adjacent frames.
// inst.rate: bank ID (0..MAX_WAVETABLE_BANKS-1) — assigned at compile time
//            by the codegen handler from the smooch("name", ...) string arg.
// in0: freq (Hz)
// in1: phase offset ([0, 1) — added to phase before the read)
// in2: tablePos (frame morph index, 0..frames.size()-1)
[[gnu::always_inline]]
inline void op_osc_wavetable(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* freq         = ctx.buffers->get(inst.inputs[0]);
    // Phase and tablePos default to silence (0) when unused — get_input_or_zero
    // routes BUFFER_UNUSED to BUFFER_ZERO so the codegen can emit smooch("name",
    // freq) without manually wiring a zero buffer.
    const float* phase_offset = get_input_or_zero(ctx, inst.inputs[1]);
    const float* tablePos     = get_input_or_zero(ctx, inst.inputs[2]);
    auto& state = ctx.states->get_or_create<SmoochState>(inst.state_id);

    const std::uint8_t bank_id = inst.rate;
    const WavetableBank* bank = (bank_id < MAX_WAVETABLE_BANKS)
                                ? ctx.wavetable_banks.banks[bank_id]
                                : nullptr;
    if (bank == nullptr || bank->frames.empty()) {
        // Bank not loaded (or out-of-range ID) — emit silence per PRD §8.c.
        std::fill_n(out, BLOCK_SIZE, 0.0f);
        return;
    }

    const std::size_t frame_count = bank->frames.size();
    const std::size_t last_frame  = frame_count - 1;
    const float nyquist           = ctx.sample_rate * 0.5f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Per PRD §8.a/b: clamp out-of-range frequencies.
        float f = freq[i];
        if (f <= 0.0f)        f = 0.01f;
        else if (f >= nyquist) f = nyquist - 1.0f;

        // 1. Phase accumulation (double for long-run stability).
        const double inc = static_cast<double>(f) *
                            static_cast<double>(ctx.inv_sample_rate);
        state.phase += inc;
        // Normal use rarely overshoots by more than 1, but keep loops to be safe.
        while (state.phase >= 1.0) state.phase -= 1.0;
        while (state.phase <  0.0) state.phase += 1.0;

        double finalPhase = state.phase + static_cast<double>(phase_offset[i]);
        finalPhase -= std::floor(finalPhase);

        // 2. Fractional mip selection. We want the LOWEST mip k such that
        // mip k's harmonic budget (1024>>k) is sufficient for frequency f
        // without aliasing. Mip k is safe up to f_max(k) = nyquist*2^k/1024.
        // The conservative formula `log2(2048 / max_harmonic)` (where
        // max_harmonic = nyquist/f) ensures `floor(mip_fractional)` lands
        // on a safe mip. The PRD §6.3 step 2 pseudocode had `1024 / ...`
        // which biases one octave too aggressive (would alias near the top
        // of each mip's nominal range).
        const float max_harmonic = nyquist / f;
        float mip_fractional = std::log2(2048.0f / std::max(max_harmonic, 1.0f));
        if (mip_fractional < 0.0f) mip_fractional = 0.0f;
        const float top_mip = static_cast<float>(MAX_MIP_LEVELS - 1);
        if (mip_fractional > top_mip) mip_fractional = top_mip;
        const std::size_t mip_low  = static_cast<std::size_t>(mip_fractional);
        const std::size_t mip_high = std::min(mip_low + 1,
                                              static_cast<std::size_t>(MAX_MIP_LEVELS - 1));
        const float mip_frac = mip_fractional - static_cast<float>(mip_low);

        // 3. Frame selection. NaN tablePos → frame 0 (PRD §8.g).
        float pos = tablePos[i];
        if (!(pos == pos)) pos = 0.0f;  // NaN check
        if (pos < 0.0f) pos = 0.0f;
        if (pos > static_cast<float>(last_frame)) pos = static_cast<float>(last_frame);
        const std::size_t frame_a = static_cast<std::size_t>(std::floor(pos));
        const std::size_t frame_b = std::min(frame_a + 1, last_frame);
        const float morph_frac = pos - static_cast<float>(frame_a);

        // 4. Per-sample read position — same for all four (frame, mip)
        // combos since the table size is constant across the pyramid.
        const float read_pos = static_cast<float>(finalPhase) *
                                static_cast<float>(WAVETABLE_SIZE);
        const int   int_pos  = static_cast<int>(read_pos);
        const float frac_pos = read_pos - static_cast<float>(int_pos);

        // 5. Equal-power weights for both axes (one cos/sin pair each).
        const float fw_a = std::cos(morph_frac * cedar::HALF_PI);
        const float fw_b = std::sin(morph_frac * cedar::HALF_PI);
        const float mw_l = std::cos(mip_frac   * cedar::HALF_PI);
        const float mw_h = std::sin(mip_frac   * cedar::HALF_PI);

        // 6. Four Niemitalo-4 reads → frame-morph blend → mip crossfade.
        const float sa_low  = wt_sample_at(bank->frames[frame_a], mip_low,  int_pos, frac_pos);
        const float sb_low  = wt_sample_at(bank->frames[frame_b], mip_low,  int_pos, frac_pos);
        const float low     = sa_low * fw_a + sb_low * fw_b;

        const float sa_high = wt_sample_at(bank->frames[frame_a], mip_high, int_pos, frac_pos);
        const float sb_high = wt_sample_at(bank->frames[frame_b], mip_high, int_pos, frac_pos);
        const float high    = sa_high * fw_a + sb_high * fw_b;

        out[i] = low * mw_l + high * mw_h;
        state.initialized = true;
    }
}
#endif  // CEDAR_NO_FFT

}  // namespace cedar

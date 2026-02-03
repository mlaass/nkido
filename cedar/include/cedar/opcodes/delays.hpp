#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// Delay Opcodes
// ============================================================================

// DELAY: Delay line with feedback
// in0: input signal
// in1: delay time (unit depends on rate field)
// in2: feedback amount (0.0-1.0, clamped to 0.99 to prevent runaway)
// in3: dry level (0xFFFF = default 0.0)
// in4: wet level (0xFFFF = default 1.0)
// rate field: time unit (0=seconds, 1=milliseconds, 2=samples)
//
// Uses linear interpolation for sub-sample delay accuracy
[[gnu::always_inline]]
inline void op_delay(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* delay_time = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    // Calculate maximum delay in samples and ensure buffer is allocated
    // Max 10 seconds regardless of unit
    float max_delay_sec = 10.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_delay_sec * ctx.sample_rate) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer) {
        // Buffer allocation failed, output silence
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
        }
        return;
    }

    // Time unit conversion factor based on rate field
    float unit_factor;
    switch (inst.rate) {
        case 0:  unit_factor = ctx.sample_rate; break;           // seconds → samples
        case 1:  unit_factor = 0.001f * ctx.sample_rate; break;  // ms → samples
        case 2:  unit_factor = 1.0f; break;                      // samples → samples
        default: unit_factor = ctx.sample_rate; break;           // default to seconds
    }

    // Smoothing coefficient for delay time changes (~20ms slew at 48kHz)
    constexpr float smooth_coeff = 0.9995f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Convert target delay time to samples using unit factor
        float target_delay = delay_time[i] * unit_factor;
        target_delay = std::clamp(target_delay, 1.0f, static_cast<float>(state.buffer_size - 2));

        // Initialize or smooth the delay time to prevent clicks
        if (!state.delay_initialized) {
            state.smoothed_delay = target_delay;
            state.delay_initialized = true;
        } else {
            // Exponential smoothing: smoothed = smoothed * coeff + target * (1 - coeff)
            state.smoothed_delay = state.smoothed_delay * smooth_coeff + target_delay * (1.0f - smooth_coeff);
        }

        float delay_samples = state.smoothed_delay;

        // Integer and fractional parts for linear interpolation
        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        // Calculate read positions (circular buffer)
        std::size_t read_pos1 = (state.write_pos + state.buffer_size - delay_int) % state.buffer_size;
        std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

        // Linear interpolation for fractional delay
        float delayed = state.buffer[read_pos1] * (1.0f - delay_frac) +
                       state.buffer[read_pos2] * delay_frac;

        // Clamp feedback to prevent runaway oscillation
        float fb = std::clamp(feedback[i], 0.0f, 0.99f);

        // Write input + feedback to buffer
        state.buffer[state.write_pos] = input[i] + delayed * fb;

        // Advance write position
        state.write_pos = (state.write_pos + 1) % state.buffer_size;

        // Output: dry * input + wet * delayed
        float dry = dry_level ? dry_level[i] : 0.0f;
        float wet = wet_level ? wet_level[i] : 1.0f;
        out[i] = input[i] * dry + delayed * wet;
    }
}

// DELAY_SYNC: Beat-synchronized delay (delay time in beats/subdivisions)
// in0: input signal
// in1: delay time in beats (e.g., 0.25 = 1/16th note at 4/4)
// in2: feedback amount (0.0-1.0)
// reserved field low byte: wet/dry mix (0-255 -> 0.0-1.0)
//
// Delay time is calculated from BPM: delay_ms = (delay_beats / bpm) * 60000
[[gnu::always_inline]]
inline void op_delay_sync(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* delay_beats = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    // rate=0 (default) means 100% wet, rate=255 means 0% wet (full dry)
    float mix = 1.0f - static_cast<float>(inst.rate) / 255.0f;

    // Calculate samples per beat
    float samples_per_beat = (60.0f / ctx.bpm) * ctx.sample_rate;

    // Maximum 4 beats delay
    float max_beats = 4.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_beats * samples_per_beat) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Convert beats to samples
        float delay_samples = delay_beats[i] * samples_per_beat;
        delay_samples = std::clamp(delay_samples, 0.0f, static_cast<float>(state.buffer_size - 2));

        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        std::size_t read_pos1 = (state.write_pos + state.buffer_size - delay_int) % state.buffer_size;
        std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

        float delayed = state.buffer[read_pos1] * (1.0f - delay_frac) +
                       state.buffer[read_pos2] * delay_frac;

        float fb = std::clamp(feedback[i], 0.0f, 0.99f);
        state.buffer[state.write_pos] = input[i] + delayed * fb;
        state.write_pos = (state.write_pos + 1) % state.buffer_size;

        out[i] = input[i] * (1.0f - mix) + delayed * mix;
    }
}

// ============================================================================
// Tap Delay Opcodes (Coordinated Pair)
// ============================================================================
// DELAY_TAP and DELAY_WRITE work together to enable configurable feedback
// processing. The pattern is:
//   1. DELAY_TAP reads from the delay buffer and caches in tap_cache
//   2. User's feedback chain processes the cached samples
//   3. DELAY_WRITE writes input + processed feedback to buffer, outputs delayed
//
// Both opcodes MUST share the same state_id to operate on the same delay buffer.

// DELAY_TAP: Read from delay line, cache for feedback processing
// in0: input signal (passed through, not used for reading)
// in1: delay time (unit depends on rate field)
// out: delayed signal (also cached in state.tap_cache)
// rate field: time unit (0=seconds, 1=milliseconds, 2=samples)
//
// Uses linear interpolation for sub-sample accuracy.
// This opcode does NOT advance write_pos - that's done by DELAY_WRITE.
[[gnu::always_inline]]
inline void op_delay_tap(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* delay_time = ctx.buffers->get(inst.inputs[1]);
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    // Calculate maximum delay in samples and ensure buffer is allocated
    // Max 10 seconds regardless of unit
    float max_delay_sec = 10.0f;
    std::size_t max_samples = static_cast<std::size_t>(max_delay_sec * ctx.sample_rate) + 1;
    max_samples = std::min(max_samples, DelayState::MAX_DELAY_SAMPLES);
    state.ensure_buffer(max_samples, ctx.arena);

    if (!state.buffer) {
        // Buffer allocation failed, output silence
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
            state.tap_cache[i] = 0.0f;
        }
        return;
    }

    // Time unit conversion factor based on rate field
    float unit_factor;
    switch (inst.rate) {
        case 0:  unit_factor = ctx.sample_rate; break;           // seconds → samples
        case 1:  unit_factor = 0.001f * ctx.sample_rate; break;  // ms → samples
        case 2:  unit_factor = 1.0f; break;                      // samples → samples
        default: unit_factor = ctx.sample_rate; break;           // default to seconds
    }

    // Smoothing coefficient for delay time changes (~20ms slew at 48kHz)
    constexpr float smooth_coeff = 0.9995f;

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Convert target delay time to samples using unit factor
        float target_delay = delay_time[i] * unit_factor;
        target_delay = std::clamp(target_delay, 1.0f, static_cast<float>(state.buffer_size - 2));

        // Initialize or smooth the delay time to prevent clicks
        if (!state.delay_initialized) {
            state.smoothed_delay = target_delay;
            state.delay_initialized = true;
        } else {
            // Exponential smoothing: smoothed = smoothed * coeff + target * (1 - coeff)
            state.smoothed_delay = state.smoothed_delay * smooth_coeff + target_delay * (1.0f - smooth_coeff);
        }

        float delay_samples = state.smoothed_delay;

        // Integer and fractional parts for linear interpolation
        std::size_t delay_int = static_cast<std::size_t>(delay_samples);
        float delay_frac = delay_samples - static_cast<float>(delay_int);

        // Calculate read positions (circular buffer)
        // Note: we read relative to current write_pos which hasn't advanced yet
        std::size_t read_pos1 = (state.write_pos + i + state.buffer_size - delay_int) % state.buffer_size;
        std::size_t read_pos2 = (read_pos1 + state.buffer_size - 1) % state.buffer_size;

        // Linear interpolation for fractional delay
        float delayed = state.buffer[read_pos1] * (1.0f - delay_frac) +
                       state.buffer[read_pos2] * delay_frac;

        // Output and cache the delayed signal
        out[i] = delayed;
        state.tap_cache[i] = delayed;
    }
}

// DELAY_WRITE: Write feedback to delay line, output delayed signal
// in0: input signal (dry signal to feed into delay)
// in1: processed feedback (output from user's feedback chain)
// in2: feedback amount (0.0-1.0, clamped to 0.99)
// in3: dry level (0xFFFF = default 0.0)
// in4: wet level (0xFFFF = default 1.0)
// out: delayed signal from tap_cache (for continuity with TAP output)
//
// Writes: buffer[write_pos] = input + processed_feedback * feedback_amount
// Then advances write_pos.
[[gnu::always_inline]]
inline void op_delay_write(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const float* processed = ctx.buffers->get(inst.inputs[1]);
    const float* feedback = ctx.buffers->get(inst.inputs[2]);
    const float* dry_level = (inst.inputs[3] != 0xFFFF) ? ctx.buffers->get(inst.inputs[3]) : nullptr;
    const float* wet_level = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;
    auto& state = ctx.states->get_or_create<DelayState>(inst.state_id);

    if (!state.buffer) {
        // Buffer not allocated (TAP should have done this), output silence
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            out[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        // Clamp feedback to prevent runaway oscillation
        float fb = std::clamp(feedback[i], 0.0f, 0.99f);

        // Write input + processed feedback to buffer
        state.buffer[state.write_pos] = input[i] + processed[i] * fb;

        // Advance write position
        state.write_pos = (state.write_pos + 1) % state.buffer_size;

        // Output: dry * input + wet * delayed
        float dry = dry_level ? dry_level[i] : 0.0f;
        float wet = wet_level ? wet_level[i] : 1.0f;
        out[i] = input[i] * dry + state.tap_cache[i] * wet;
    }
}

}  // namespace cedar

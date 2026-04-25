#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"

namespace cedar {

// EDGE_OP: edge-detection / sample-and-hold / counter, mode-dispatched on inst.rate
//   rate=0 sah(in, trig)            — classic sample-and-hold (was op_sah)
//   rate=1 gateup(sig)              — 1.0 on rising edge of inputs[0], else 0.0
//   rate=2 gatedown(sig)            — 1.0 on falling edge of inputs[0], else 0.0
//   rate=3 counter(trig, reset?, start?) — increment on trig rising edge,
//                                     reset to start (or 0) on reset rising edge.
//                                     reset wins if both fire on the same sample.
[[gnu::always_inline]]
inline void op_edge(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<EdgeState>(inst.state_id);
    float* out = ctx.buffers->get(inst.out_buffer);

    switch (inst.rate) {
        case 0: {
            // sah(in, trig)
            const float* input = ctx.buffers->get(inst.inputs[0]);
            const float* trig = ctx.buffers->get(inst.inputs[1]);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                if (state.prev_trigger <= 0.0f && trig[i] > 0.0f) {
                    state.held_value = input[i];
                }
                state.prev_trigger = trig[i];
                out[i] = state.held_value;
            }
            break;
        }
        case 1: {
            // gateup(sig)
            const float* sig = ctx.buffers->get(inst.inputs[0]);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                out[i] = (state.prev_trigger <= 0.0f && sig[i] > 0.0f) ? 1.0f : 0.0f;
                state.prev_trigger = sig[i];
            }
            break;
        }
        case 2: {
            // gatedown(sig)
            const float* sig = ctx.buffers->get(inst.inputs[0]);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                out[i] = (state.prev_trigger > 0.0f && sig[i] <= 0.0f) ? 1.0f : 0.0f;
                state.prev_trigger = sig[i];
            }
            break;
        }
        case 3: {
            // counter(trig, reset?, start?)
            const float* trig = ctx.buffers->get(inst.inputs[0]);
            const float* reset = (inst.inputs[1] != BUFFER_UNUSED)
                ? ctx.buffers->get(inst.inputs[1]) : nullptr;
            const float* start = (inst.inputs[2] != BUFFER_UNUSED)
                ? ctx.buffers->get(inst.inputs[2]) : nullptr;
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                if (reset && state.prev_reset_trigger <= 0.0f && reset[i] > 0.0f) {
                    state.held_value = start ? start[i] : 0.0f;
                } else if (state.prev_trigger <= 0.0f && trig[i] > 0.0f) {
                    state.held_value = state.held_value + 1.0f;
                }
                state.prev_trigger = trig[i];
                if (reset) state.prev_reset_trigger = reset[i];
                out[i] = state.held_value;
            }
            break;
        }
        default:
            // Unknown mode: emit silence so we don't propagate uninitialized memory
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = 0.0f;
            break;
    }
}

}  // namespace cedar

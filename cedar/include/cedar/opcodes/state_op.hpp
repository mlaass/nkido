#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"

namespace cedar {

// STATE_OP: user state cell I/O, mode-dispatched on inst.rate.
//
// Each call site of `state(init)` in Akkado allocates one CellState slot in the
// state pool, keyed by the same FNV-1a path-hash used by every other stateful
// builtin. `.get()` and `.set(v)` then route to that slot via inst.state_id.
//
//   rate=0 init  — first execution writes inputs[0][0] to slot, sets initialized=true.
//                  Subsequent executions are no-ops (slot value preserved across blocks
//                  AND across hot-swap edits — see PRD §9). Output is broadcast of slot.
//   rate=1 load  — output is broadcast of slot value. No input reads.
//   rate=2 store — scan inputs[0] and write the LATEST sample whose value differs
//                  from the slot's value at the start of this block. Samples that
//                  equal the start-of-block value are no-ops, so an `idx.set(
//                  select(gateup(t), idx.get()+dir, idx.get()))` pattern survives
//                  the rising-edge sample even when it isn't the final one. For a
//                  constant-across-the-block input this collapses to the obvious
//                  "write that constant" behavior. Output is broadcast of the new
//                  slot value (so set() can be used in expression position).
[[gnu::always_inline]]
inline void op_state(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<CellState>(inst.state_id);
    float* out = ctx.buffers->get(inst.out_buffer);

    switch (inst.rate) {
        case 0: {
            if (!state.initialized) {
                const float* init = ctx.buffers->get(inst.inputs[0]);
                state.value = init[0];
                state.initialized = true;
            }
            break;
        }
        case 1: {
            // Pure load — slot already up to date.
            break;
        }
        case 2: {
            const float* in = ctx.buffers->get(inst.inputs[0]);
            const float initial = state.value;
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                if (in[i] != initial) state.value = in[i];
            }
            state.initialized = true;
            break;
        }
        default:
            // Unknown mode: emit silence
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = 0.0f;
            return;
    }

    const float v = state.value;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = v;
}

}  // namespace cedar

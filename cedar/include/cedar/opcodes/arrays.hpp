#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include <algorithm>
#include <cmath>

namespace cedar {

// ARRAY_PACK: Pack up to 5 scalars into an array buffer
// rate = element count (1-5)
// in0..in4 = scalar buffers (takes first sample from each)
// out = array buffer with elements at indices 0..rate-1
[[gnu::always_inline]]
inline void op_array_pack(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const std::uint8_t count = inst.rate;

    // Pack scalar values into array positions
    for (std::uint8_t i = 0; i < count && i < 5; ++i) {
        if (inst.inputs[i] != BUFFER_UNUSED) {
            out[i] = ctx.buffers->get(inst.inputs[i])[0];  // Take sample 0
        } else {
            out[i] = 0.0f;
        }
    }
}

// ARRAY_INDEX: Per-sample indexing with wrap or clamp
// in0 = array buffer
// in1 = index buffer (0-based indices, one per sample)
// in2 = array length (encoded as buffer index, value in first sample)
// rate: 0=wrap, 1=clamp
// out[i] = arr[index[i]] for each of 128 samples
[[gnu::always_inline]]
inline void op_array_index(ExecutionContext& ctx, const Instruction& inst) {
    const float* arr = ctx.buffers->get(inst.inputs[0]);
    const float* idx = ctx.buffers->get(inst.inputs[1]);
    float* out = ctx.buffers->get(inst.out_buffer);

    // Array length passed via inputs[2] as a constant buffer
    const int length = static_cast<int>(ctx.buffers->get(inst.inputs[2])[0]);
    if (length <= 0) {
        std::fill_n(out, BLOCK_SIZE, 0.0f);
        return;
    }

    const bool clamp_mode = (inst.rate == 1);

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        int j = static_cast<int>(std::floor(idx[i]));
        if (clamp_mode) {
            j = std::clamp(j, 0, length - 1);
        } else {
            // Wrap mode - handle negative indices too
            j = ((j % length) + length) % length;
        }
        out[i] = arr[j];
    }
}

// ARRAY_UNPACK: Extract single element from array
// in0 = array buffer
// rate = element index (0-127)
// out = scalar buffer (all 128 samples filled with that element)
[[gnu::always_inline]]
inline void op_array_unpack(ExecutionContext& ctx, const Instruction& inst) {
    const float* arr = ctx.buffers->get(inst.inputs[0]);
    float* out = ctx.buffers->get(inst.out_buffer);

    const std::uint8_t index = inst.rate;
    float value = (index < BLOCK_SIZE) ? arr[index] : 0.0f;
    std::fill_n(out, BLOCK_SIZE, value);
}

// ARRAY_LEN: Fill buffer with array length
// rate = array length
// out = scalar buffer filled with length value
[[gnu::always_inline]]
inline void op_array_len(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    float length = static_cast<float>(inst.rate);
    std::fill_n(out, BLOCK_SIZE, length);
}

// ARRAY_SLICE: Extract a slice of an array
// in0 = array buffer
// in1 = start index buffer (takes first sample)
// in2 = end index buffer (takes first sample, exclusive)
// rate = source array length
// out = sliced array buffer
[[gnu::always_inline]]
inline void op_array_slice(ExecutionContext& ctx, const Instruction& inst) {
    const float* arr = ctx.buffers->get(inst.inputs[0]);
    float* out = ctx.buffers->get(inst.out_buffer);

    const int src_len = static_cast<int>(inst.rate);
    int start = static_cast<int>(ctx.buffers->get(inst.inputs[1])[0]);
    int end = static_cast<int>(ctx.buffers->get(inst.inputs[2])[0]);

    // Clamp bounds
    start = std::clamp(start, 0, src_len);
    end = std::clamp(end, start, src_len);

    int slice_len = end - start;
    for (int i = 0; i < slice_len && i < static_cast<int>(BLOCK_SIZE); ++i) {
        out[i] = arr[start + i];
    }
}

// ARRAY_CONCAT: Concatenate two arrays
// in0 = first array buffer
// in1 = second array buffer
// rate = length of first array
// inputs[2] = length of second array (as buffer, take first sample)
// out = concatenated array
[[gnu::always_inline]]
inline void op_array_concat(ExecutionContext& ctx, const Instruction& inst) {
    const float* arr_a = ctx.buffers->get(inst.inputs[0]);
    const float* arr_b = ctx.buffers->get(inst.inputs[1]);
    float* out = ctx.buffers->get(inst.out_buffer);

    const int len_a = static_cast<int>(inst.rate);
    const int len_b = static_cast<int>(ctx.buffers->get(inst.inputs[2])[0]);

    // Copy first array
    int out_idx = 0;
    for (int i = 0; i < len_a && out_idx < static_cast<int>(BLOCK_SIZE); ++i, ++out_idx) {
        out[out_idx] = arr_a[i];
    }
    // Copy second array
    for (int i = 0; i < len_b && out_idx < static_cast<int>(BLOCK_SIZE); ++i, ++out_idx) {
        out[out_idx] = arr_b[i];
    }
}

// ARRAY_PUSH: Append element to array (functional - returns new array)
// in0 = array buffer
// in1 = element to append (takes first sample)
// rate = current array length
// out = new array with element appended
[[gnu::always_inline]]
inline void op_array_push(ExecutionContext& ctx, const Instruction& inst) {
    const float* arr = ctx.buffers->get(inst.inputs[0]);
    float* out = ctx.buffers->get(inst.out_buffer);

    const int len = static_cast<int>(inst.rate);
    float elem = ctx.buffers->get(inst.inputs[1])[0];

    // Copy existing elements
    for (int i = 0; i < len && i < static_cast<int>(BLOCK_SIZE) - 1; ++i) {
        out[i] = arr[i];
    }
    // Append new element
    if (len < static_cast<int>(BLOCK_SIZE)) {
        out[len] = elem;
    }
}

// ARRAY_SUM: Sum all array elements
// in0 = array buffer
// rate = array length
// out = scalar buffer filled with sum
[[gnu::always_inline]]
inline void op_array_sum(ExecutionContext& ctx, const Instruction& inst) {
    const float* arr = ctx.buffers->get(inst.inputs[0]);
    float* out = ctx.buffers->get(inst.out_buffer);

    const int len = static_cast<int>(inst.rate);
    float sum = 0.0f;
    for (int i = 0; i < len && i < static_cast<int>(BLOCK_SIZE); ++i) {
        sum += arr[i];
    }
    std::fill_n(out, BLOCK_SIZE, sum);
}

// ARRAY_REVERSE: Reverse array order
// in0 = array buffer
// rate = array length
// out = reversed array
[[gnu::always_inline]]
inline void op_array_reverse(ExecutionContext& ctx, const Instruction& inst) {
    const float* arr = ctx.buffers->get(inst.inputs[0]);
    float* out = ctx.buffers->get(inst.out_buffer);

    const int len = static_cast<int>(inst.rate);
    for (int i = 0; i < len && i < static_cast<int>(BLOCK_SIZE); ++i) {
        out[i] = arr[len - 1 - i];
    }
}

// ARRAY_FILL: Create array filled with a single value
// in0 = value buffer (takes first sample)
// rate = array length
// out = array filled with value at indices 0..length-1
[[gnu::always_inline]]
inline void op_array_fill(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    float value = ctx.buffers->get(inst.inputs[0])[0];
    const int len = static_cast<int>(inst.rate);

    for (int i = 0; i < len && i < static_cast<int>(BLOCK_SIZE); ++i) {
        out[i] = value;
    }
}

}  // namespace cedar

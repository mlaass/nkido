#pragma once

#include "cedar/wavetable/bank.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace cedar {

struct WavData;

/// Fill `frame` with the 11-level band-limited mip pyramid built from
/// `sourceData` (one cycle of waveform, exactly WAVETABLE_SIZE samples).
/// Source conditioning per PRD §5.1: DC removal, FFT, fundamental-phase
/// alignment, raised-cosine taper (4 bins) per mip, IFFT, 1/N scale,
/// RMS normalization to source RMS. `sourceData` is taken by value and
/// mutated internally.
void generate_wavetable_mips(WavetableFrame& frame,
                             std::array<float, WAVETABLE_SIZE> sourceData);

/// Build a complete bank from raw mono samples whose total length is a
/// multiple of WAVETABLE_SIZE. Each consecutive 2048-sample chunk becomes
/// one frame. Returns nullptr on validation failure (writes a human-readable
/// reason into `*error_out` when provided).
std::shared_ptr<WavetableBank> build_bank_from_samples(
    const std::string& name,
    const float* samples,
    std::size_t num_samples,
    std::string* error_out = nullptr);

/// Build a bank from an already-decoded WavData. WAV must be mono and its
/// sample count a multiple of WAVETABLE_SIZE. Returns nullptr on failure.
std::shared_ptr<WavetableBank> build_bank_from_wav(
    const std::string& name,
    const WavData& wav,
    std::string* error_out = nullptr);

}  // namespace cedar

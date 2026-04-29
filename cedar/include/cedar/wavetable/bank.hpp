#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cedar {

/// Number of samples in one cycle of one wavetable frame at every mip level.
/// Matches the Serum/Surge XT/Vital convention; required to be a power of two
/// so the audio loop can use a bitwise mask for index wrapping.
constexpr int WAVETABLE_SIZE = 2048;

/// Mip pyramid depth. Level 0 keeps all 1024 harmonics (N/2); each subsequent
/// level halves the harmonic count via spectral filtering. Level 10 keeps only
/// the fundamental.
constexpr int MAX_MIP_LEVELS = 11;

/// One source-frame's mip pyramid: 11 octave-spaced band-limited copies of
/// the same 2048-sample waveform. The source has been DC-stripped and
/// fundamental-phase-aligned, then each row is the IFFT'd, RMS-normalized
/// band-limited waveform (see preprocessor.cpp).
struct WavetableFrame {
    std::array<std::array<float, WAVETABLE_SIZE>, MAX_MIP_LEVELS> mipMaps;
};

/// One loaded wavetable bank — immutable during audio playback, shared by
/// reference across all OSC_WAVETABLE voices.
struct WavetableBank {
    std::vector<WavetableFrame> frames;
    int tableSize    = WAVETABLE_SIZE;
    int numMipLevels = MAX_MIP_LEVELS;
    std::string name;
};

}  // namespace cedar

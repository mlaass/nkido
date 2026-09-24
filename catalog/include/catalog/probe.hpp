#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace catalog {

// Cheap per-file metadata probe (prd-studio-sample-catalog Phase 0).
//
// WAV and FLAC are probed from their headers without decoding audio data;
// OGG and MP3 fall back to a full decode through cedar::AudioDecoder (their
// containers have no cheap authoritative frame count). `bit_depth` is only
// known for WAV/FLAC — lossy formats have none.
struct ProbeResult {
    bool success = false;
    std::string error;  // set when !success

    double duration_s = 0.0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::optional<std::uint16_t> bit_depth;
    std::uint64_t num_frames = 0;
};

// Probes by extension (case-insensitive): .wav, .flac, .ogg, .mp3.
// Unknown extension => !success with error "unsupported extension".
ProbeResult probe_file(const std::string& path);

// Byte-level entry points (used by probe_file and the tests).
ProbeResult probe_wav(const std::uint8_t* data, std::size_t size);
ProbeResult probe_flac(const std::uint8_t* data, std::size_t size);

}  // namespace catalog

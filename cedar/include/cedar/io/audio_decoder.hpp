#pragma once

#include "cedar/io/buffer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cedar {

enum class AudioFormat {
    Unknown,
    WAV,
    OGG,
    FLAC,
    MP3
};

struct DecodedAudio {
    std::vector<float> samples;  // Interleaved
    std::uint32_t sample_rate = 0;
    std::uint32_t num_frames = 0;
    std::uint16_t channels = 0;
    bool success = false;
    std::string error;
};

class AudioDecoder {
public:
    /// Detect audio format from magic bytes
    static AudioFormat detect_format(MemoryView data);

    /// Auto-detect format and decode
    static DecodedAudio decode(MemoryView data);

    /// Format-specific decoders
    static DecodedAudio decode_wav(MemoryView data);
    static DecodedAudio decode_ogg(MemoryView data);
    static DecodedAudio decode_flac(MemoryView data);
    static DecodedAudio decode_mp3(MemoryView data);
};

}  // namespace cedar

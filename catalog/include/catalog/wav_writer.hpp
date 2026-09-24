#pragma once

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace catalog {

// Minimal PCM16 WAV writer — shared by the synthetic-corpus generator, the
// tests and the perf harness. Not a general-purpose encoder.
inline bool write_wav16(const std::string& path, const std::vector<std::int16_t>& interleaved,
                        std::uint32_t sample_rate, std::uint16_t channels) {
    if (channels == 0) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    const std::uint32_t data_size = static_cast<std::uint32_t>(interleaved.size() * 2);
    const std::uint32_t byte_rate = sample_rate * channels * 2;
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);

    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    u32(36 + data_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    u32(16);
    u16(1);  // PCM
    u16(channels);
    u32(sample_rate);
    u32(byte_rate);
    u16(block_align);
    u16(16);
    f.write("data", 4);
    u32(data_size);
    f.write(reinterpret_cast<const char*>(interleaved.data()),
            static_cast<std::streamsize>(data_size));
    return f.good();
}

// A short sine burst — deterministic content for fixtures.
inline std::vector<std::int16_t> sine_pcm16(std::uint32_t num_frames, std::uint16_t channels,
                                            double freq_hz, std::uint32_t sample_rate) {
    std::vector<std::int16_t> out(static_cast<std::size_t>(num_frames) * channels);
    for (std::uint32_t i = 0; i < num_frames; ++i) {
        const double s = std::sin(2.0 * 3.14159265358979323846 * freq_hz * i / sample_rate);
        const auto v = static_cast<std::int16_t>(s * 12000.0);
        for (std::uint16_t c = 0; c < channels; ++c) {
            out[static_cast<std::size_t>(i) * channels + c] = v;
        }
    }
    return out;
}

}  // namespace catalog

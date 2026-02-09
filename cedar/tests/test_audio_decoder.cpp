#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/io/audio_decoder.hpp>
#include <cedar/io/buffer.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Helper: create test WAV data in memory
// ============================================================================

static std::vector<std::uint8_t> make_test_wav(int num_frames, int channels, int sample_rate) {
    std::vector<std::uint8_t> data;

    auto push16 = [&](std::uint16_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
    };
    auto push32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) data.push_back((v >> (i * 8)) & 0xFF);
    };

    int total_samples = num_frames * channels;
    std::uint32_t data_size = total_samples * 2;  // 16-bit PCM

    // RIFF header
    data.push_back('R'); data.push_back('I'); data.push_back('F'); data.push_back('F');
    push32(36 + data_size);
    data.push_back('W'); data.push_back('A'); data.push_back('V'); data.push_back('E');

    // fmt chunk
    data.push_back('f'); data.push_back('m'); data.push_back('t'); data.push_back(' ');
    push32(16);              // chunk size
    push16(1);               // PCM format
    push16(static_cast<std::uint16_t>(channels));
    push32(static_cast<std::uint32_t>(sample_rate));
    push32(static_cast<std::uint32_t>(sample_rate * channels * 2));  // byte rate
    push16(static_cast<std::uint16_t>(channels * 2));  // block align
    push16(16);              // bits per sample

    // data chunk
    data.push_back('d'); data.push_back('a'); data.push_back('t'); data.push_back('a');
    push32(data_size);

    // Sample data: sine wave
    for (int s = 0; s < num_frames; ++s) {
        float phase = static_cast<float>(s) / static_cast<float>(num_frames);
        float value = std::sin(phase * 6.28318f);
        auto sample = static_cast<std::int16_t>(value * 32767.0f);

        for (int c = 0; c < channels; ++c) {
            data.push_back(static_cast<std::uint8_t>(sample & 0xFF));
            data.push_back(static_cast<std::uint8_t>((sample >> 8) & 0xFF));
        }
    }

    return data;
}

// ============================================================================
// Format detection [audio_decoder]
// ============================================================================

TEST_CASE("AudioDecoder format detection", "[audio_decoder]") {
    SECTION("detect WAV") {
        auto wav = make_test_wav(10, 1, 44100);
        MemoryView view(wav);
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::WAV);
    }

    SECTION("detect OGG") {
        std::vector<std::uint8_t> data = {'O', 'g', 'g', 'S', 0, 0, 0, 0};
        MemoryView view(data);
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::OGG);
    }

    SECTION("detect FLAC") {
        std::vector<std::uint8_t> data = {'f', 'L', 'a', 'C', 0, 0, 0, 0};
        MemoryView view(data);
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::FLAC);
    }

    SECTION("detect MP3 with ID3 tag") {
        std::vector<std::uint8_t> data = {'I', 'D', '3', 0x04, 0, 0, 0, 0};
        MemoryView view(data);
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::MP3);
    }

    SECTION("detect MP3 with frame sync") {
        std::vector<std::uint8_t> data = {0xFF, 0xFB, 0x90, 0x00};
        MemoryView view(data);
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::MP3);
    }

    SECTION("unknown format") {
        std::vector<std::uint8_t> data = {0x00, 0x01, 0x02, 0x03};
        MemoryView view(data);
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::Unknown);
    }

    SECTION("empty data") {
        MemoryView view;
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::Unknown);
    }

    SECTION("too small data") {
        std::vector<std::uint8_t> data = {0x01, 0x02};
        MemoryView view(data);
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::Unknown);
    }
}

// ============================================================================
// WAV decoding [audio_decoder]
// ============================================================================

TEST_CASE("AudioDecoder WAV decode", "[audio_decoder]") {
    SECTION("decode mono WAV") {
        auto wav = make_test_wav(100, 1, 44100);
        MemoryView view(wav);

        auto result = AudioDecoder::decode(view);

        REQUIRE(result.success);
        CHECK(result.num_frames == 100);
        CHECK(result.channels == 1);
        CHECK(result.sample_rate == 44100);
        CHECK(result.samples.size() == 100);
    }

    SECTION("decode stereo WAV") {
        auto wav = make_test_wav(50, 2, 48000);
        MemoryView view(wav);

        auto result = AudioDecoder::decode(view);

        REQUIRE(result.success);
        CHECK(result.num_frames == 50);
        CHECK(result.channels == 2);
        CHECK(result.sample_rate == 48000);
        CHECK(result.samples.size() == 100);  // 50 frames * 2 channels
    }

    SECTION("decode_wav directly") {
        auto wav = make_test_wav(200, 1, 22050);
        MemoryView view(wav);

        auto result = AudioDecoder::decode_wav(view);

        REQUIRE(result.success);
        CHECK(result.num_frames == 200);
        CHECK(result.sample_rate == 22050);
    }

    SECTION("round-trip data integrity") {
        // Create a WAV with known values
        auto wav = make_test_wav(100, 1, 44100);
        MemoryView view(wav);

        auto result = AudioDecoder::decode(view);
        REQUIRE(result.success);

        // First sample should be approximately sin(0) = 0
        CHECK_THAT(result.samples[0], WithinAbs(0.0f, 0.001f));

        // Sample at 25% should be approximately sin(pi/2) = 1.0
        // (at frame 25 of 100: phase = 0.25 * 2*pi = pi/2)
        CHECK_THAT(result.samples[25], WithinAbs(1.0f, 0.01f));
    }
}

// ============================================================================
// Error handling [audio_decoder]
// ============================================================================

TEST_CASE("AudioDecoder error handling", "[audio_decoder]") {
    SECTION("garbage data") {
        std::vector<std::uint8_t> data = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        MemoryView view(data);

        auto result = AudioDecoder::decode(view);
        CHECK_FALSE(result.success);
    }

    SECTION("truncated WAV") {
        auto wav = make_test_wav(100, 1, 44100);
        wav.resize(30);  // Truncate to less than header
        MemoryView view(wav);

        auto result = AudioDecoder::decode(view);
        CHECK_FALSE(result.success);
    }

    SECTION("zero-length data") {
        MemoryView view;
        auto result = AudioDecoder::decode(view);
        CHECK_FALSE(result.success);
    }

    SECTION("RIFF header but not WAVE") {
        std::vector<std::uint8_t> data = {
            'R', 'I', 'F', 'F',
            0x00, 0x00, 0x00, 0x00,
            'A', 'V', 'I', ' '  // AVI, not WAVE
        };
        MemoryView view(data);

        // Should detect as unknown (not WAV since no WAVE marker)
        CHECK(AudioDecoder::detect_format(view) == AudioFormat::Unknown);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>
#include <vector>

#include "catalog/probe.hpp"
#include "catalog/wav_writer.hpp"
#include "test_helpers.hpp"

using catalog::probe_file;
using catalog::probe_flac;
using catalog::probe_wav;
using catalog::test::TempDir;
using catalog::test::write_bytes;
using Catch::Matchers::WithinAbs;

TEST_CASE("wav probe reads fmt without decoding", "[probe]") {
    TempDir tmp;
    const auto pcm = catalog::sine_pcm16(4410, 2, 440.0, 44100);
    REQUIRE(catalog::write_wav16(tmp.str("s.wav"), pcm, 44100, 2));

    const auto r = probe_file(tmp.str("s.wav"));
    REQUIRE(r.success);
    CHECK(r.sample_rate == 44100);
    CHECK(r.channels == 2);
    REQUIRE(r.bit_depth.has_value());
    CHECK(*r.bit_depth == 16);
    CHECK(r.num_frames == 4410);
    CHECK_THAT(r.duration_s, WithinAbs(0.1, 1e-9));
}

TEST_CASE("wav probe skips leading junk chunks", "[probe]") {
    // RIFF/WAVE with a JUNK chunk before fmt — common in exported WAVs.
    std::vector<std::uint8_t> wav;
    auto push32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) wav.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto push16 = [&](std::uint16_t v) {
        wav.push_back(static_cast<std::uint8_t>(v & 0xFF));
        wav.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto push_tag = [&](const char* t) {
        wav.insert(wav.end(), t, t + 4);
    };
    push_tag("RIFF");
    push32(0);  // size — probe does not depend on it
    push_tag("WAVE");
    push_tag("JUNK");
    push32(8);
    for (int i = 0; i < 8; ++i) wav.push_back(0);
    push_tag("fmt ");
    push32(16);
    push16(1);      // PCM
    push16(1);      // mono
    push32(22050);  // sample rate
    push32(22050 * 2);
    push16(2);   // block align
    push16(16);  // bits
    push_tag("data");
    push32(2 * 100);  // 100 frames

    const auto r = probe_wav(wav.data(), wav.size());
    REQUIRE(r.success);
    CHECK(r.sample_rate == 22050);
    CHECK(r.channels == 1);
    CHECK(r.num_frames == 100);
}

TEST_CASE("wav probe rejects garbage and truncation", "[probe]") {
    const char* garbage = "this is not audio at all, definitely not";
    auto r = probe_wav(reinterpret_cast<const std::uint8_t*>(garbage), std::strlen(garbage));
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.error.empty());

    // Valid RIFF/WAVE prefix but no chunks.
    const char* empty_riff = "RIFF\x04\x00\x00\x00WAVE";
    r = probe_wav(reinterpret_cast<const std::uint8_t*>(empty_riff), 12);
    CHECK_FALSE(r.success);
}

TEST_CASE("flac probe reads STREAMINFO", "[probe]") {
    // Hand-built fLaC header: 44100 Hz, stereo, 16-bit, 88200 total samples.
    std::vector<std::uint8_t> flac = {'f', 'L', 'a', 'C', 0x00, 0x00, 0x00, 0x22};
    std::vector<std::uint8_t> si(34, 0);
    si[10] = 0x0A;  // sample rate 44100 = 0x0AC44, 20 bits
    si[11] = 0xC4;
    si[12] = 0x42;  // rate low nibble=4, channels-1=1 -> bits 3..1 = 001, bps hi bit 0
    si[13] = 0xF0;  // bps-1=15 -> low 4 bits in high nibble; total-samples hi nibble 0
    si[14] = 0x00;
    si[15] = 0x01;
    si[16] = 0x58;
    si[17] = 0x88;  // 0x00015888 = 88200
    flac.insert(flac.end(), si.begin(), si.end());

    const auto r = probe_flac(flac.data(), flac.size());
    REQUIRE(r.success);
    CHECK(r.sample_rate == 44100);
    CHECK(r.channels == 2);
    REQUIRE(r.bit_depth.has_value());
    CHECK(*r.bit_depth == 16);
    CHECK(r.num_frames == 88200);
    CHECK_THAT(r.duration_s, WithinAbs(2.0, 1e-9));

    // The same bytes through the extension-dispatching path.
    TempDir tmp;
    write_bytes(tmp.str("s.flac"), flac.data(), flac.size());
    const auto r2 = probe_file(tmp.str("s.flac"));
    REQUIRE(r2.success);
    CHECK(r2.sample_rate == 44100);
}

TEST_CASE("flac probe rejects non-flac", "[probe]") {
    const char* nope = "OggS this is something else entirely padding padding";
    const auto r = probe_flac(reinterpret_cast<const std::uint8_t*>(nope), std::strlen(nope));
    CHECK_FALSE(r.success);
}

TEST_CASE("unsupported extension fails cleanly", "[probe]") {
    const auto r = probe_file("/nonexistent/sample.aiff");
    CHECK_FALSE(r.success);
    CHECK(r.error == "unsupported extension");
}

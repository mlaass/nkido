#include <catch2/catch_test_macros.hpp>

#include <cedar/audio/soundfont.hpp>
#include <cedar/vm/sample_bank.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// Locate a real SF3/SF2 fixture. Prefer the smaller TimGM6mb.sf3 shipped
// alongside the web app. Tests requiring a successful load skip gracefully
// when no fixture is found, so the suite stays portable.
static fs::path find_sf_fixture() {
    // Walk up from CWD looking for web/static/soundfonts/*.sf3 / *.sf2.
    fs::path cur = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        for (const auto& candidate : {
                 cur / "web" / "static" / "soundfonts" / "TimGM6mb.sf3",
                 cur / "web" / "static" / "soundfonts" / "FluidR3Mono_GM.sf3",
             }) {
            if (fs::exists(candidate)) return candidate;
        }
        if (cur.has_parent_path() && cur.parent_path() != cur) {
            cur = cur.parent_path();
        } else {
            break;
        }
    }
    return {};
}

static std::vector<std::uint8_t> read_file_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE(in.good());
    auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::uint8_t> buf(sz);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

TEST_CASE("SoundFontRegistry rejects invalid input", "[soundfont][registry]") {
    cedar::SoundFontRegistry reg;
    cedar::SampleBank bank;

    SECTION("null data returns -1") {
        CHECK(reg.load_from_memory(nullptr, 1024, "x", bank) == -1);
        CHECK(reg.size() == 0);
    }

    SECTION("zero size returns -1") {
        std::uint8_t dummy[4]{};
        CHECK(reg.load_from_memory(dummy, 0, "x", bank) == -1);
        CHECK(reg.size() == 0);
    }

    SECTION("negative size returns -1") {
        std::uint8_t dummy[4]{};
        CHECK(reg.load_from_memory(dummy, -1, "x", bank) == -1);
        CHECK(reg.size() == 0);
    }

    SECTION("non-RIFF data returns -1") {
        // 12 bytes so the header check actually runs.
        const char garbage[] = "not-a-riff\0";
        CHECK(reg.load_from_memory(garbage, sizeof(garbage), "x", bank) == -1);
        CHECK(reg.size() == 0);
    }

    SECTION("RIFF but not sfbk returns -1") {
        // Valid RIFF header, but type WAVE instead of sfbk.
        std::uint8_t hdr[16] = {
            'R','I','F','F',  0,0,0,0,
            'W','A','V','E',  0,0,0,0
        };
        CHECK(reg.load_from_memory(hdr, sizeof(hdr), "x", bank) == -1);
        CHECK(reg.size() == 0);
    }
}

TEST_CASE("SoundFontRegistry deduplicates by name", "[soundfont][registry][dedup]") {
    auto fixture = find_sf_fixture();
    if (fixture.empty()) {
        SKIP("No SoundFont fixture found (looked under web/static/soundfonts/)");
    }

    auto bytes = read_file_bytes(fixture);
    REQUIRE(bytes.size() > 12);

    cedar::SoundFontRegistry reg;
    cedar::SampleBank bank;

    SECTION("loading the same name twice returns the same ID") {
        int first = reg.load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                          "gm", bank);
        REQUIRE(first >= 0);
        REQUIRE(reg.size() == 1);

        // Second call with the same name short-circuits via the dedup loop.
        // Use deliberately invalid data to prove the parser is never reached.
        std::uint8_t junk[16] = {0};
        int second = reg.load_from_memory(junk, sizeof(junk), "gm", bank);

        CHECK(second == first);
        CHECK(reg.size() == 1);  // no new bank added
    }

    SECTION("loading a different name returns a different ID") {
        int first = reg.load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                          "gm", bank);
        REQUIRE(first >= 0);

        int second = reg.load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                           "piano", bank);
        REQUIRE(second >= 0);
        CHECK(second != first);
        CHECK(reg.size() == 2);
    }

    SECTION("dedup short-circuits before parsing (no SampleBank growth)") {
        // First load extends sample_bank with the SF2's samples.
        int first = reg.load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                          "gm", bank);
        REQUIRE(first >= 0);
        std::size_t samples_after_first = bank.size();

        // Second load with same name but fresh-looking bytes: must not grow
        // the sample bank, because the dedup short-circuit prevents parsing.
        int second = reg.load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                           "gm", bank);
        CHECK(second == first);
        CHECK(bank.size() == samples_after_first);
    }
}

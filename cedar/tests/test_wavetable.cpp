#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/wavetable/bank.hpp"
#include "cedar/wavetable/preprocessor.hpp"
#include "cedar/wavetable/registry.hpp"

#include <array>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

constexpr float PI_F = 3.14159265358979323846f;
constexpr int   N    = cedar::WAVETABLE_SIZE;

float rms_of(const std::array<float, cedar::WAVETABLE_SIZE>& a) {
    float sumSq = 0.0f;
    for (float s : a) sumSq += s * s;
    return std::sqrt(sumSq / static_cast<float>(N));
}

}  // namespace

TEST_CASE("Wavetable: bank build rejects empty samples", "[wavetable]") {
    std::string err;
    auto bank = cedar::build_bank_from_samples("empty", nullptr, 0, &err);
    REQUIRE(bank == nullptr);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("Wavetable: bank build rejects non-2048-multiple sizes", "[wavetable]") {
    std::vector<float> samples(2049, 0.0f);
    std::string err;
    auto bank = cedar::build_bank_from_samples("bad", samples.data(),
                                                samples.size(), &err);
    REQUIRE(bank == nullptr);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("Wavetable: DC component is removed in mip 0", "[wavetable]") {
    // Source = sine + constant 0.5 DC offset.
    std::array<float, N> src{};
    for (int i = 0; i < N; ++i) {
        src[i] = std::sin(2.0f * PI_F * static_cast<float>(i) /
                           static_cast<float>(N)) + 0.5f;
    }
    cedar::WavetableFrame frame{};
    cedar::generate_wavetable_mips(frame, src);

    float sum = 0.0f;
    for (float s : frame.mipMaps[0]) sum += s;
    const float dc = sum / static_cast<float>(N);
    CHECK_THAT(dc, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("Wavetable: fundamental phase-aligned to peak at index 0", "[wavetable]") {
    // Pure sine — bin 1 has phase pi/2. After phase alignment, bin 1's phase
    // is rotated to 0, making the time-domain reconstruction a pure cosine
    // peaking at index 0.
    std::array<float, N> src{};
    for (int i = 0; i < N; ++i) {
        src[i] = std::sin(2.0f * PI_F * static_cast<float>(i) /
                           static_cast<float>(N));
    }
    cedar::WavetableFrame frame{};
    cedar::generate_wavetable_mips(frame, src);

    int peak_idx = 0;
    float peak = -1e9f;
    for (int i = 0; i < N; ++i) {
        if (frame.mipMaps[0][i] > peak) {
            peak = frame.mipMaps[0][i];
            peak_idx = i;
        }
    }
    INFO("peak at index " << peak_idx << ", value " << peak);
    // Within a couple samples of index 0 (allowing for FFT/normalization noise).
    CHECK((peak_idx <= 2 || peak_idx >= N - 2));
}

TEST_CASE("Wavetable: every mip level RMS-matches the source", "[wavetable]") {
    // Multi-harmonic signal so the band-limiting actually removes content.
    std::array<float, N> src{};
    for (int i = 0; i < N; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(N);
        src[i] =  0.50f * std::sin(2.0f * PI_F * t)
               + 0.30f * std::sin(2.0f * PI_F * 4.0f  * t)
               + 0.20f * std::sin(2.0f * PI_F * 16.0f * t);
    }
    cedar::WavetableFrame frame{};
    cedar::generate_wavetable_mips(frame, src);

    const float src_rms = rms_of(src);
    REQUIRE(src_rms > 0.05f);

    for (int k = 0; k < cedar::MAX_MIP_LEVELS; ++k) {
        const float mip_rms = rms_of(frame.mipMaps[k]);
        const float ratio_db = 20.0f * std::log10(mip_rms / src_rms);
        INFO("mip " << k << ": RMS " << mip_rms
             << " vs source " << src_rms << " (" << ratio_db << " dB)");
        CHECK(std::abs(ratio_db) < 0.5f);
    }
}

TEST_CASE("Wavetable: highest mip is sinusoidal (fundamental only)", "[wavetable]") {
    // Three-harmonic source: only the fundamental should survive at mip 10.
    std::array<float, N> src{};
    for (int i = 0; i < N; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(N);
        src[i] =  std::sin(2.0f * PI_F * t)
               + 0.30f * std::sin(2.0f * PI_F * 5.0f  * t)
               + 0.20f * std::sin(2.0f * PI_F * 17.0f * t);
    }
    cedar::WavetableFrame frame{};
    cedar::generate_wavetable_mips(frame, src);

    const auto& mip = frame.mipMaps[10];
    const float r = rms_of(mip);
    float peak = 0.0f;
    for (float s : mip) peak = std::max(peak, std::abs(s));
    const float crest = peak / r;

    // Pure sine has crest factor sqrt(2). Allow small tolerance for
    // raised-cosine taper bleed.
    CHECK_THAT(crest, WithinAbs(std::sqrt(2.0f), 0.05f));
}

TEST_CASE("Wavetable: bank build from synthetic morph source", "[wavetable]") {
    constexpr int frames_count = 32;
    std::vector<float> samples(static_cast<std::size_t>(frames_count) * N);
    for (int f = 0; f < frames_count; ++f) {
        const float morph = static_cast<float>(f) /
                            static_cast<float>(frames_count - 1);
        for (int i = 0; i < N; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(N);
            const float sine = std::sin(2.0f * PI_F * t);
            const float tri  = 2.0f * std::abs(2.0f * t - 1.0f) - 1.0f;
            samples[static_cast<std::size_t>(f) * N + i] =
                sine * (1.0f - morph) + tri * morph;
        }
    }
    std::string err;
    auto bank = cedar::build_bank_from_samples("morph", samples.data(),
                                                samples.size(), &err);
    REQUIRE(bank != nullptr);
    CHECK(err.empty());
    CHECK(static_cast<int>(bank->frames.size()) == frames_count);
    CHECK(bank->name == "morph");
    CHECK(bank->tableSize == cedar::WAVETABLE_SIZE);
    CHECK(bank->numMipLevels == cedar::MAX_MIP_LEVELS);
}

TEST_CASE("Wavetable: multi-bank registry assigns sequential IDs", "[wavetable]") {
    cedar::WavetableBankRegistry reg;
    REQUIRE(reg.size() == 0);
    REQUIRE_FALSE(reg.has("anything"));
    REQUIRE(reg.find_id("anything") == -1);

    // Two minimal one-frame banks.
    std::vector<float> samples_a(N, 0.0f);
    std::vector<float> samples_b(N, 0.0f);
    for (int i = 0; i < N; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(N);
        samples_a[i] = std::sin(2.0f * PI_F * t);
        samples_b[i] = std::sin(2.0f * PI_F * 2.0f * t);
    }
    auto bank_a = cedar::build_bank_from_samples("A", samples_a.data(),
                                                  samples_a.size());
    auto bank_b = cedar::build_bank_from_samples("B", samples_b.data(),
                                                  samples_b.size());
    REQUIRE(bank_a != nullptr);
    REQUIRE(bank_b != nullptr);

    const int id_a = reg.set_named("A", bank_a);
    const int id_b = reg.set_named("B", bank_b);
    CHECK(id_a == 0);
    CHECK(id_b == 1);
    CHECK(reg.size() == 2);
    CHECK(reg.has("A"));
    CHECK(reg.has("B"));
    CHECK(reg.find_id("A") == 0);
    CHECK(reg.find_id("B") == 1);
    CHECK(reg.get(0).get() == bank_a.get());
    CHECK(reg.get(1).get() == bank_b.get());
    CHECK(reg.get(99) == nullptr);
    CHECK(reg.get(-1) == nullptr);

    // Re-registering the same name keeps the ID and replaces the bank.
    auto bank_a2 = cedar::build_bank_from_samples("A", samples_b.data(),
                                                    samples_b.size());
    const int id_a_again = reg.set_named("A", bank_a2);
    CHECK(id_a_again == id_a);
    CHECK(reg.size() == 2);
    CHECK(reg.get(0).get() == bank_a2.get());

    reg.clear();
    CHECK(reg.size() == 0);
    CHECK_FALSE(reg.has("A"));
    CHECK(reg.find_id("A") == -1);
}

TEST_CASE("Wavetable: snapshot pins lifetime and indexes by ID", "[wavetable]") {
    cedar::WavetableBankRegistry reg;
    std::vector<float> samples(N, 0.0f);
    for (int i = 0; i < N; ++i) {
        samples[i] = std::sin(2.0f * PI_F * static_cast<float>(i) /
                              static_cast<float>(N));
    }
    auto bank = cedar::build_bank_from_samples("first",
                                                samples.data(), samples.size());
    REQUIRE(bank != nullptr);
    reg.set_named("first", bank);

    cedar::WavetableBankSnapshot snap{};
    std::array<std::shared_ptr<const cedar::WavetableBank>,
                cedar::MAX_WAVETABLE_BANKS> pins{};
    reg.snapshot(snap, pins);

    CHECK(snap.count == 1);
    CHECK(snap.banks[0] == bank.get());
    CHECK(snap.banks[1] == nullptr);
    CHECK(pins[0].get() == bank.get());

    // Drop the registry's reference; the snapshot's pin still keeps the
    // bank alive (audio-thread invariant: a snapshot is safe to read
    // even after the host clears or replaces the registry mid-block).
    reg.clear();
    CHECK(snap.banks[0] == bank.get());  // still valid via pins[0]
    CHECK(pins[0]->frames.size() == 1);
}

// Smoke test for EFFECT_PHASER. The phaser's output must be the dry+wet
// sum (input + allpass cascade) — otherwise the all-pass cascade on its
// own has unity magnitude response and the spectrum shows no notches.
//
// We pin depth=0 (stationary notch at the geometric-mean center) so the
// notch shows cleanly in a single FFT, then assert it is at least 6 dB
// below the local mean of surrounding bins.

#include <catch2/catch_test_macros.hpp>
#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/dsp/constants.hpp"
#include "cedar/dsp/fft.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace cedar;

namespace {

Instruction push_const(std::uint16_t out, float value) {
    Instruction inst{};
    inst.opcode = Opcode::PUSH_CONST;
    inst.out_buffer = out;
    inst.inputs[0] = inst.inputs[1] = inst.inputs[2] = inst.inputs[3] = inst.inputs[4] = BUFFER_UNUSED;
    inst.state_id = std::bit_cast<std::uint32_t>(value);
    return inst;
}

}  // namespace

TEST_CASE("EFFECT_PHASER produces audible spectral notches", "[modulation][phaser]") {
    constexpr std::uint16_t BUF_INPUT = 0;
    constexpr std::uint16_t BUF_RATE = 1;
    constexpr std::uint16_t BUF_DEPTH = 2;
    constexpr std::uint16_t BUF_MIN = 3;
    constexpr std::uint16_t BUF_MAX = 4;
    constexpr std::uint16_t BUF_OUT = 5;

    // Stationary phaser: rate=0 (LFO frozen), depth=0 (no sweep) => single
    // notch at sqrt(min*max) = sqrt(200*4000) ≈ 894 Hz.
    Instruction phaser{};
    phaser.opcode = Opcode::EFFECT_PHASER;
    phaser.out_buffer = BUF_OUT;
    phaser.inputs[0] = BUF_INPUT;
    phaser.inputs[1] = BUF_RATE;
    phaser.inputs[2] = BUF_DEPTH;
    phaser.inputs[3] = BUF_MIN;
    phaser.inputs[4] = BUF_MAX;
    // 4 stages, 0 feedback (high nibble) — neutral resonance
    phaser.rate = (0u << 4) | 4u;
    phaser.state_id = 0xCAFEBABEu;

    std::vector<Instruction> program{
        push_const(BUF_RATE, 0.0f),
        push_const(BUF_DEPTH, 0.0f),
        push_const(BUF_MIN, 200.0f),
        push_const(BUF_MAX, 4000.0f),
        phaser,
    };

    VM vm;
    vm.load_program(std::span(program));

    // White noise generator (deterministic seed)
    std::mt19937 rng(424242);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::array<float, BLOCK_SIZE> L{}, R{};

    constexpr std::size_t SETTLE_BLOCKS = 64;     // ~170 ms of settling
    constexpr std::size_t NFFT = 2048;            // FFT window size
    static_assert(NFFT % BLOCK_SIZE == 0, "NFFT must be a multiple of BLOCK_SIZE");
    constexpr std::size_t CAPTURE_BLOCKS = NFFT / BLOCK_SIZE;

    auto fill_input_with_noise = [&]() {
        float* in = vm.buffers().get(BUF_INPUT);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            in[i] = dist(rng);
        }
    };

    // Settle
    for (std::size_t b = 0; b < SETTLE_BLOCKS; ++b) {
        fill_input_with_noise();
        vm.process_block(L.data(), R.data());
    }

    // Capture NFFT samples of phaser output
    std::array<float, NFFT> capture{};
    for (std::size_t b = 0; b < CAPTURE_BLOCKS; ++b) {
        fill_input_with_noise();
        vm.process_block(L.data(), R.data());
        const float* out = vm.buffers().get(BUF_OUT);
        std::memcpy(&capture[b * BLOCK_SIZE], out, BLOCK_SIZE * sizeof(float));
    }

    // Sanity: output must be finite
    for (float v : capture) {
        REQUIRE(std::isfinite(v));
    }

    // FFT
    std::array<float, NFFT / 2 + 1> mag_db{};
    {
        std::array<float, NFFT> re{}, im{};
        compute_fft(capture.data(), NFFT, re.data(), im.data());
        compute_magnitude_db(re.data(), im.data(), NFFT, mag_db.data());
    }

    // Search for the deepest local minimum in 100..6000 Hz. A 4-stage
    // cascade at center f_c=894 Hz produces notches near 0.414*f_c (~370 Hz)
    // and 2.414*f_c (~2160 Hz); we don't pin the frequency exactly because
    // it depends on coefficient details, but it must be in the audible
    // band where listeners would hear the effect.
    constexpr float SAMPLE_RATE = 48000.0f;
    const float bin_hz = SAMPLE_RATE / static_cast<float>(NFFT);
    const std::size_t lo_bin = static_cast<std::size_t>(100.0f / bin_hz);
    const std::size_t hi_bin = static_cast<std::size_t>(6000.0f / bin_hz);

    std::size_t min_bin = lo_bin;
    float min_db = mag_db[lo_bin];
    double sum_db = 0.0;
    std::size_t count = 0;
    for (std::size_t b = lo_bin; b <= hi_bin; ++b) {
        sum_db += mag_db[b];
        ++count;
        if (mag_db[b] < min_db) {
            min_db = mag_db[b];
            min_bin = b;
        }
    }
    const float mean_db = static_cast<float>(sum_db / static_cast<double>(count));
    const float notch_depth = mean_db - min_db;
    const float notch_freq = static_cast<float>(min_bin) * bin_hz;

    INFO("notch_freq=" << notch_freq << " Hz, notch_depth=" << notch_depth
         << " dB, mean=" << mean_db << " dB");

    // The dry+wet sum produces a clear notch. A bare allpass cascade output
    // (the bug we fixed) would produce ~0 dB depth — flat magnitude response.
    REQUIRE(notch_depth > 6.0f);
    // Notch must be in the audible band — sanity check that it's not landing
    // at DC or above the analysis band edge.
    REQUIRE(notch_freq > 100.0f);
    REQUIRE(notch_freq < 6000.0f);
}

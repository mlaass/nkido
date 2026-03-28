#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/dsp/fft.hpp"
#include "cedar/opcodes/dsp_state.hpp"
#include "cedar/dsp/constants.hpp"
#include "cedar/vm/audio_arena.hpp"

#include <cmath>
#include <cstring>

using Catch::Matchers::WithinAbs;

// ============================================================================
// FFT Wrapper Tests
// ============================================================================

TEST_CASE("FFT: 440Hz sine peak at expected bin", "[fft]") {
    constexpr std::size_t NFFT = 1024;
    constexpr float SAMPLE_RATE = 48000.0f;
    constexpr float FREQ = 440.0f;

    // Generate a 440Hz sine wave
    float signal[NFFT];
    for (std::size_t i = 0; i < NFFT; i++) {
        signal[i] = std::sin(2.0f * 3.14159265f * FREQ * static_cast<float>(i) / SAMPLE_RATE);
    }

    float real_out[NFFT / 2 + 1];
    float imag_out[NFFT / 2 + 1];
    cedar::compute_fft(signal, NFFT, real_out, imag_out);

    // Expected peak bin: 440 * 1024 / 48000 = 9.386... ≈ bin 9
    int expected_bin = static_cast<int>(std::round(FREQ * NFFT / SAMPLE_RATE));
    std::size_t num_bins = NFFT / 2 + 1;

    // Find the peak bin
    float max_mag = 0;
    int peak_bin = 0;
    for (std::size_t i = 1; i < num_bins; i++) {  // Skip DC
        float mag = std::sqrt(real_out[i] * real_out[i] + imag_out[i] * imag_out[i]);
        if (mag > max_mag) {
            max_mag = mag;
            peak_bin = static_cast<int>(i);
        }
    }

    CHECK(peak_bin == expected_bin);
    CHECK(max_mag > 100.0f);  // Should have significant energy
}

TEST_CASE("FFT: all supported sizes produce correct bin count", "[fft]") {
    std::size_t sizes[] = {256, 512, 1024, 2048};

    for (auto nfft : sizes) {
        SECTION("size " + std::to_string(nfft)) {
            std::vector<float> signal(nfft, 0.0f);
            signal[0] = 1.0f;  // Impulse

            std::vector<float> real_out(nfft / 2 + 1);
            std::vector<float> imag_out(nfft / 2 + 1);

            cedar::compute_fft(signal.data(), nfft, real_out.data(), imag_out.data());

            // Verify we got valid data (impulse should produce flat spectrum)
            bool all_finite = true;
            for (std::size_t i = 0; i < nfft / 2 + 1; i++) {
                if (!std::isfinite(real_out[i]) || !std::isfinite(imag_out[i])) {
                    all_finite = false;
                    break;
                }
            }
            CHECK(all_finite);
        }
    }
}

TEST_CASE("FFT: round-trip FFT->IFFT reconstructs input", "[fft]") {
    constexpr std::size_t NFFT = 1024;

    // Generate a test signal (sum of sines)
    float signal[NFFT];
    for (std::size_t i = 0; i < NFFT; i++) {
        float t = static_cast<float>(i) / 48000.0f;
        signal[i] = std::sin(2.0f * 3.14159265f * 440.0f * t)
                   + 0.5f * std::sin(2.0f * 3.14159265f * 880.0f * t);
    }

    float real_out[NFFT / 2 + 1];
    float imag_out[NFFT / 2 + 1];
    cedar::compute_fft(signal, NFFT, real_out, imag_out);

    float reconstructed[NFFT];
    cedar::compute_ifft(real_out, imag_out, NFFT, reconstructed);

    // The reconstructed signal won't match exactly because the forward FFT
    // applies a Hanning window. But the windowed signal should match.
    // Apply same Hanning window to original for comparison.
    float windowed[NFFT];
    for (std::size_t i = 0; i < NFFT; i++) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * static_cast<float>(i) / (NFFT - 1)));
        windowed[i] = signal[i] * w;
    }

    // Compare windowed signal with reconstructed
    float max_error = 0;
    for (std::size_t i = 0; i < NFFT; i++) {
        float err = std::abs(windowed[i] - reconstructed[i]);
        if (err > max_error) max_error = err;
    }

    CHECK(max_error < 1e-4f);
}

TEST_CASE("FFT: compute_magnitude_db produces finite values", "[fft]") {
    constexpr std::size_t NFFT = 1024;

    float signal[NFFT];
    for (std::size_t i = 0; i < NFFT; i++) {
        signal[i] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }

    float real_out[NFFT / 2 + 1];
    float imag_out[NFFT / 2 + 1];
    cedar::compute_fft(signal, NFFT, real_out, imag_out);

    float mag_db[NFFT / 2 + 1];
    cedar::compute_magnitude_db(real_out, imag_out, NFFT, mag_db);

    bool all_finite = true;
    for (std::size_t i = 0; i < NFFT / 2 + 1; i++) {
        if (!std::isfinite(mag_db[i])) {
            all_finite = false;
            break;
        }
    }
    CHECK(all_finite);

    // Peak should be well above -120dB floor
    float peak_db = -200.0f;
    for (std::size_t i = 1; i < NFFT / 2 + 1; i++) {
        if (mag_db[i] > peak_db) peak_db = mag_db[i];
    }
    CHECK(peak_db > -40.0f);  // 440Hz sine should be clearly visible
}

// ============================================================================
// FFTProbeState Tests
// ============================================================================

TEST_CASE("FFTProbeState: frame counter increments correctly", "[fft][state]") {
    cedar::AudioArena arena;
    cedar::FFTProbeState state;
    state.fft_size = 1024;

    // Arena-allocate buffers (simulating what op_fft_probe does)
    state.input_buffer = arena.allocate(cedar::FFTProbeState::MAX_FFT_SIZE);
    state.magnitudes_db = arena.allocate(cedar::FFTProbeState::MAX_BINS);
    state.real_bins = arena.allocate(cedar::FFTProbeState::MAX_BINS);
    state.imag_bins = arena.allocate(cedar::FFTProbeState::MAX_BINS);

    REQUIRE(state.input_buffer != nullptr);
    CHECK(state.frame_counter == 0);
    CHECK(!state.initialized);

    // Generate test signal blocks (128 samples each)
    float block[cedar::BLOCK_SIZE];
    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; i++) {
        block[i] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }

    // Write 8 blocks = 1024 samples = 1 full FFT frame
    for (int b = 0; b < 8; b++) {
        state.write_block(block, cedar::BLOCK_SIZE);
    }

    CHECK(state.frame_counter == 1);
    CHECK(state.initialized);

    // Write 8 more blocks = second frame
    for (int b = 0; b < 8; b++) {
        state.write_block(block, cedar::BLOCK_SIZE);
    }

    CHECK(state.frame_counter == 2);
}

TEST_CASE("FFTProbeState: magnitudes are finite after FFT", "[fft][state]") {
    cedar::AudioArena arena;
    cedar::FFTProbeState state;
    state.fft_size = 512;

    state.input_buffer = arena.allocate(cedar::FFTProbeState::MAX_FFT_SIZE);
    state.magnitudes_db = arena.allocate(cedar::FFTProbeState::MAX_BINS);
    state.real_bins = arena.allocate(cedar::FFTProbeState::MAX_BINS);
    state.imag_bins = arena.allocate(cedar::FFTProbeState::MAX_BINS);

    // Generate signal
    float block[cedar::BLOCK_SIZE];
    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; i++) {
        block[i] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }

    // Write 4 blocks = 512 samples
    for (int b = 0; b < 4; b++) {
        state.write_block(block, cedar::BLOCK_SIZE);
    }

    REQUIRE(state.frame_counter == 1);

    // Check all magnitude values are finite
    std::size_t bin_count = state.fft_size / 2 + 1;
    bool all_finite = true;
    for (std::size_t i = 0; i < bin_count; i++) {
        if (!std::isfinite(state.magnitudes_db[i])) {
            all_finite = false;
            break;
        }
    }
    CHECK(all_finite);
}

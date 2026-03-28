#include "cedar/dsp/fft.hpp"
#include "cedar/opcodes/dsp_state.hpp"

#include <cmath>
#include <cstring>

// Include kissfft implementation directly (same pattern as audio_decoder.cpp with stb_vorbis)
extern "C" {
#include "kissfft/kiss_fft.c"
#include "kissfft/kiss_fftr.c"
}

namespace cedar {

// ============================================================================
// Cached FFT configs and Hanning windows
// ============================================================================

// Supported sizes indexed by log2: index 8=256, 9=512, 10=1024, 11=2048
static constexpr int MIN_LOG2 = 8;
static constexpr int MAX_LOG2 = 11;
static constexpr int NUM_SIZES = MAX_LOG2 - MIN_LOG2 + 1;

static kiss_fftr_cfg g_fwd_configs[NUM_SIZES] = {};
static kiss_fftr_cfg g_inv_configs[NUM_SIZES] = {};

// Pre-computed Hanning windows
static float g_hanning_256[256];
static float g_hanning_512[512];
static float g_hanning_1024[1024];
static float g_hanning_2048[2048];

static float* g_hanning_ptrs[NUM_SIZES] = {
    g_hanning_256, g_hanning_512, g_hanning_1024, g_hanning_2048
};

static bool g_initialized = false;

static int log2_size(std::size_t nfft) {
    // __builtin_ctz gives trailing zeros = log2 for power-of-2
    return __builtin_ctz(static_cast<unsigned>(nfft));
}

static void ensure_initialized() {
    if (g_initialized) return;

    // Allocate configs for each supported size
    for (int i = 0; i < NUM_SIZES; ++i) {
        int nfft = 1 << (MIN_LOG2 + i);
        g_fwd_configs[i] = kiss_fftr_alloc(nfft, 0, nullptr, nullptr);
        g_inv_configs[i] = kiss_fftr_alloc(nfft, 1, nullptr, nullptr);

        // Compute Hanning window
        float* win = g_hanning_ptrs[i];
        for (int j = 0; j < nfft; ++j) {
            win[j] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979323846f * static_cast<float>(j) / static_cast<float>(nfft - 1)));
        }
    }

    g_initialized = true;
}

// ============================================================================
// Public API
// ============================================================================

void compute_fft(const float* time_domain, std::size_t nfft,
                 float* real_out, float* imag_out) {
    ensure_initialized();

    int idx = log2_size(nfft) - MIN_LOG2;
    if (idx < 0 || idx >= NUM_SIZES) return;

    kiss_fftr_cfg cfg = g_fwd_configs[idx];
    const float* window = g_hanning_ptrs[idx];

    // Apply Hanning window to a temporary buffer
    // Use stack for small sizes, static for large to avoid stack overflow in WASM
    float windowed[2048];
    for (std::size_t i = 0; i < nfft; ++i) {
        windowed[i] = time_domain[i] * window[i];
    }

    // Compute FFT - kissfft real FFT produces nfft/2+1 complex bins
    std::size_t num_bins = nfft / 2 + 1;
    kiss_fft_cpx out[1025];  // MAX_BINS
    kiss_fftr(cfg, windowed, out);

    // Deinterleave complex output
    for (std::size_t i = 0; i < num_bins; ++i) {
        real_out[i] = out[i].r;
        imag_out[i] = out[i].i;
    }
}

void compute_ifft(const float* real_in, const float* imag_in,
                  std::size_t nfft, float* time_domain_out) {
    ensure_initialized();

    int idx = log2_size(nfft) - MIN_LOG2;
    if (idx < 0 || idx >= NUM_SIZES) return;

    kiss_fftr_cfg cfg = g_inv_configs[idx];

    // Interleave into complex input
    std::size_t num_bins = nfft / 2 + 1;
    kiss_fft_cpx in[1025];  // MAX_BINS
    for (std::size_t i = 0; i < num_bins; ++i) {
        in[i].r = real_in[i];
        in[i].i = imag_in[i];
    }

    // Compute inverse FFT
    kiss_fftri(cfg, in, time_domain_out);

    // Normalize by 1/nfft (kissfft does not normalize)
    float scale = 1.0f / static_cast<float>(nfft);
    for (std::size_t i = 0; i < nfft; ++i) {
        time_domain_out[i] *= scale;
    }
}

void compute_magnitude_db(const float* real, const float* imag,
                          std::size_t nfft, float* magnitudes_db_out) {
    std::size_t num_bins = nfft / 2 + 1;
    float scale = 1.0f / static_cast<float>(nfft);

    for (std::size_t i = 0; i < num_bins; ++i) {
        float r = real[i];
        float im = imag[i];
        float mag = std::sqrt(r * r + im * im) * scale;

        // Convert to dB, floor at -120dB to avoid -inf
        if (mag < 1e-6f) {
            magnitudes_db_out[i] = -120.0f;
        } else {
            magnitudes_db_out[i] = 20.0f * std::log10(mag);
        }
    }
}

// ============================================================================
// FFTProbeState implementation
// ============================================================================

void FFTProbeState::write_block(const float* samples, std::size_t count) {
    if (!input_buffer) return;  // Not yet arena-allocated

    for (std::size_t i = 0; i < count; ++i) {
        input_buffer[write_pos] = samples[i];
        write_pos++;
        if (write_pos >= fft_size) {
            compute_fft(input_buffer, fft_size, real_bins, imag_bins);
            compute_magnitude_db(real_bins, imag_bins, fft_size, magnitudes_db);
            frame_counter++;
            write_pos = 0;
            initialized = true;
        }
    }
}

}  // namespace cedar

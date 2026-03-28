#pragma once

#include <cstddef>

namespace cedar {

// Supported FFT sizes: 256, 512, 1024, 2048
// Configs and Hanning windows are cached per size for zero-allocation reuse.

// Forward FFT: time-domain -> frequency-domain
// Applies Hanning window, returns nfft/2+1 complex bins
void compute_fft(const float* time_domain, std::size_t nfft,
                 float* real_out, float* imag_out);

// Inverse FFT: frequency-domain -> time-domain
// Takes nfft/2+1 complex bins, returns nfft time-domain samples
void compute_ifft(const float* real_in, const float* imag_in,
                  std::size_t nfft, float* time_domain_out);

// Convenience: compute magnitude spectrum in dB
// Returns nfft/2+1 values in dB (20*log10(magnitude/nfft))
void compute_magnitude_db(const float* real, const float* imag,
                          std::size_t nfft, float* magnitudes_db_out);

}  // namespace cedar

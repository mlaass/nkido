/// Offline wavetable preprocessor — converts raw 2048-sample frames into
/// band-limited mip pyramids. Runs on the host thread at wt_load() time.
///
/// Implements PRD §5.1 verbatim. Calls kissfft directly (NOT cedar/dsp/fft.hpp,
/// which applies a Hanning window — wrong for wavetable generation, where we
/// need a lossless round-trip). The kissfft .c sources are linked via
/// cedar/src/dsp/fft.cpp (which #includes them in an extern "C" block); we
/// only need the public header declarations here.

#include "cedar/wavetable/preprocessor.hpp"
#include "cedar/audio/wav_loader.hpp"

#include "kissfft/kiss_fftr.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cedar {

namespace {

constexpr std::size_t TAPER_WIDTH = 4;
constexpr float       PI_F        = 3.14159265358979323846f;

}  // namespace

void generate_wavetable_mips(WavetableFrame& frame,
                              std::array<float, WAVETABLE_SIZE> sourceData) {
    constexpr std::size_t N = WAVETABLE_SIZE;
    constexpr std::size_t NUM_BINS = N / 2 + 1;

    kiss_fftr_cfg fwd = kiss_fftr_alloc(static_cast<int>(N), 0, nullptr, nullptr);
    kiss_fftr_cfg inv = kiss_fftr_alloc(static_cast<int>(N), 1, nullptr, nullptr);

    // Step 2: remove DC offset from the source.
    float mean = 0.0f;
    for (float s : sourceData) mean += s;
    mean /= static_cast<float>(N);
    for (auto& s : sourceData) s -= mean;

    // Step 3: forward FFT to get the full complex spectrum.
    std::vector<kiss_fft_cpx> spectrum(NUM_BINS);
    kiss_fftr(fwd, sourceData.data(), spectrum.data());

    // Step 4: phase-align the spectrum so the fundamental's cosine peaks at
    // sample index 0. Skip if the fundamental is below detection floor —
    // such frames can't be morphed cleanly anyway and will reconstruct as
    // whatever phase the source happened to have.
    const float fundMag = std::sqrt(spectrum[1].r * spectrum[1].r +
                                    spectrum[1].i * spectrum[1].i);
    if (fundMag > 1e-6f) {
        const float theta = std::atan2(spectrum[1].i, spectrum[1].r);
        for (std::size_t bin = 1; bin < NUM_BINS; ++bin) {
            const float ang = -static_cast<float>(bin) * theta;
            const float c = std::cos(ang);
            const float s = std::sin(ang);
            const float r_new = spectrum[bin].r * c - spectrum[bin].i * s;
            const float i_new = spectrum[bin].r * s + spectrum[bin].i * c;
            spectrum[bin].r = r_new;
            spectrum[bin].i = i_new;
        }
    }

    // Step 5: cache source RMS for per-mip amplitude normalization.
    float srcSumSq = 0.0f;
    for (float s : sourceData) srcSumSq += s * s;
    float srcRMS = std::sqrt(srcSumSq / static_cast<float>(N));
    if (srcRMS < 1e-9f) srcRMS = 1.0f;

    std::vector<kiss_fft_cpx> filtered(NUM_BINS);

    for (std::size_t k = 0; k < static_cast<std::size_t>(MAX_MIP_LEVELS); ++k) {
        // Step 6.1: copy the conditioned source spectrum.
        filtered = spectrum;

        // Step 6.2-6.3: spectral cutoff with raised-cosine taper. The cutoff
        // bin is the highest harmonic this mip retains. The PRD pseudocode
        // (§5.2) used `bin >= cutoffBin` which kills the fundamental at
        // mip 10 — the prose in §5.1.6.3 says "above cutoffBin, zero out",
        // i.e. exclusive, which is what we implement here. The taper covers
        // the last TAPER_WIDTH bins below cutoffBin (gain 1 → 0); RMS
        // normalization in step 6.6 compensates for the taper's amplitude
        // loss.
        const std::size_t cutoffBin  = (N / 2) >> k;            // 1024, 512, ..., 1
        const std::size_t taperStart = (cutoffBin > TAPER_WIDTH)
                                       ? cutoffBin - TAPER_WIDTH
                                       : 0;
        for (std::size_t bin = taperStart; bin < NUM_BINS; ++bin) {
            float gain;
            if (bin > cutoffBin) {
                gain = 0.0f;
            } else {
                const float t = static_cast<float>(bin - taperStart) /
                                static_cast<float>(TAPER_WIDTH);
                gain = 0.5f * (1.0f + std::cos(t * PI_F));
            }
            filtered[bin].r *= gain;
            filtered[bin].i *= gain;
        }

        // Step 6.4-6.5: inverse FFT and normalize by 1/N (kissfft does not
        // normalize internally).
        std::array<float, N> output{};
        kiss_fftri(inv, filtered.data(), output.data());
        const float scale = 1.0f / static_cast<float>(N);
        for (auto& s : output) s *= scale;

        // Step 6.6: RMS-normalize the band-limited waveform back to the
        // source's RMS. RMS preserves perceived loudness across mip
        // boundaries better than peak-matching.
        float sumSq = 0.0f;
        for (float s : output) sumSq += s * s;
        const float mipRMS = std::sqrt(sumSq / static_cast<float>(N));
        if (mipRMS > 1e-9f) {
            const float gain = srcRMS / mipRMS;
            for (auto& s : output) s *= gain;
        }

        frame.mipMaps[k] = output;
    }

    kiss_fftr_free(fwd);
    kiss_fftr_free(inv);
}

std::shared_ptr<WavetableBank> build_bank_from_samples(
    const std::string& name,
    const float* samples,
    std::size_t num_samples,
    std::string* error_out) {
    if (samples == nullptr || num_samples == 0) {
        if (error_out) {
            *error_out = "wavetable WAV is empty";
        }
        return nullptr;
    }
    if ((num_samples % WAVETABLE_SIZE) != 0) {
        if (error_out) {
            *error_out = "wavetable WAV length must be a multiple of "
                       + std::to_string(WAVETABLE_SIZE) + " samples (got "
                       + std::to_string(num_samples) + ")";
        }
        return nullptr;
    }

    auto bank = std::make_shared<WavetableBank>();
    bank->name = name;
    bank->tableSize    = WAVETABLE_SIZE;
    bank->numMipLevels = MAX_MIP_LEVELS;

    const std::size_t num_frames = num_samples / WAVETABLE_SIZE;
    bank->frames.resize(num_frames);

    for (std::size_t f = 0; f < num_frames; ++f) {
        std::array<float, WAVETABLE_SIZE> src{};
        std::copy(samples + f * WAVETABLE_SIZE,
                  samples + (f + 1) * WAVETABLE_SIZE,
                  src.begin());
        generate_wavetable_mips(bank->frames[f], src);
    }
    return bank;
}

std::shared_ptr<WavetableBank> build_bank_from_wav(
    const std::string& name,
    const WavData& wav,
    std::string* error_out) {
    if (!wav.success) {
        if (error_out) *error_out = wav.error_message;
        return nullptr;
    }
    if (wav.channels != 1) {
        if (error_out) {
            *error_out = "wavetable WAV must be mono (got "
                       + std::to_string(wav.channels) + " channels)";
        }
        return nullptr;
    }
    return build_bank_from_samples(name, wav.samples.data(),
                                    wav.samples.size(), error_out);
}

}  // namespace cedar

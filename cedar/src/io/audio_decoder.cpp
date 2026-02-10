#include "cedar/io/audio_decoder.hpp"
#include "cedar/audio/wav_loader.hpp"

// Implementation defines for single-header libraries

// stb_vorbis: OGG Vorbis decoder
// We include the .c file directly since it's a single-translation-unit library.
// Suppress warnings from third-party code.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "stb_vorbis.c"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// dr_flac: FLAC decoder
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include "dr_flac.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// minimp3: MP3 decoder
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#include "minimp3.h"
#include "minimp3_ex.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstring>

namespace cedar {

AudioFormat AudioDecoder::detect_format(MemoryView data) {
    if (data.size < 4) return AudioFormat::Unknown;

    // WAV: starts with "RIFF"
    if (data.size >= 12 &&
        data.data[0] == 'R' && data.data[1] == 'I' &&
        data.data[2] == 'F' && data.data[3] == 'F' &&
        data.data[8] == 'W' && data.data[9] == 'A' &&
        data.data[10] == 'V' && data.data[11] == 'E') {
        return AudioFormat::WAV;
    }

    // OGG: starts with "OggS"
    if (data.data[0] == 'O' && data.data[1] == 'g' &&
        data.data[2] == 'g' && data.data[3] == 'S') {
        return AudioFormat::OGG;
    }

    // FLAC: starts with "fLaC"
    if (data.data[0] == 'f' && data.data[1] == 'L' &&
        data.data[2] == 'a' && data.data[3] == 'C') {
        return AudioFormat::FLAC;
    }

    // MP3: ID3 tag or frame sync
    if (data.size >= 3) {
        // ID3v2 tag
        if (data.data[0] == 'I' && data.data[1] == 'D' && data.data[2] == '3') {
            return AudioFormat::MP3;
        }
        // MPEG frame sync (0xFF followed by 0xE0+ for various MPEG versions)
        if (data.data[0] == 0xFF && (data.data[1] & 0xE0) == 0xE0) {
            return AudioFormat::MP3;
        }
    }

    return AudioFormat::Unknown;
}

DecodedAudio AudioDecoder::decode(MemoryView data) {
    auto format = detect_format(data);

    switch (format) {
        case AudioFormat::WAV:
            return decode_wav(data);
        case AudioFormat::OGG:
            return decode_ogg(data);
        case AudioFormat::FLAC:
            return decode_flac(data);
        case AudioFormat::MP3:
            return decode_mp3(data);
        default: {
            DecodedAudio result;
            result.error = "Unknown or unsupported audio format";
            return result;
        }
    }
}

DecodedAudio AudioDecoder::decode_wav(MemoryView data) {
    auto wav = WavLoader::load_from_memory(data.data, data.size);

    DecodedAudio result;
    if (!wav.success) {
        result.error = wav.error_message;
        return result;
    }

    result.samples = std::move(wav.samples);
    result.sample_rate = wav.sample_rate;
    result.num_frames = wav.num_frames;
    result.channels = wav.channels;
    result.success = true;
    return result;
}

DecodedAudio AudioDecoder::decode_ogg(MemoryView data) {
    DecodedAudio result;

    int channels = 0;
    int sample_rate = 0;
    short* output = nullptr;

    int num_samples = stb_vorbis_decode_memory(
        data.data, static_cast<int>(data.size),
        &channels, &sample_rate, &output);

    if (num_samples <= 0 || !output) {
        result.error = "Failed to decode OGG Vorbis data";
        if (output) std::free(output);
        return result;
    }

    // Convert int16 to float, interleaved
    std::size_t total = static_cast<std::size_t>(num_samples) * static_cast<std::size_t>(channels);
    result.samples.resize(total);
    for (std::size_t i = 0; i < total; ++i) {
        result.samples[i] = output[i] / 32768.0f;
    }

    std::free(output);

    result.sample_rate = static_cast<std::uint32_t>(sample_rate);
    result.num_frames = static_cast<std::uint32_t>(num_samples);
    result.channels = static_cast<std::uint16_t>(channels);
    result.success = true;
    return result;
}

DecodedAudio AudioDecoder::decode_flac(MemoryView data) {
    DecodedAudio result;

    unsigned int channels = 0;
    unsigned int sample_rate = 0;
    drflac_uint64 total_frames = 0;

    float* output = drflac_open_memory_and_read_pcm_frames_f32(
        data.data, data.size,
        &channels, &sample_rate, &total_frames,
        nullptr);

    if (!output) {
        result.error = "Failed to decode FLAC data";
        return result;
    }

    std::size_t total = static_cast<std::size_t>(total_frames) * channels;
    result.samples.assign(output, output + total);
    drflac_free(output, nullptr);

    result.sample_rate = sample_rate;
    result.num_frames = static_cast<std::uint32_t>(total_frames);
    result.channels = static_cast<std::uint16_t>(channels);
    result.success = true;
    return result;
}

DecodedAudio AudioDecoder::decode_mp3(MemoryView data) {
    DecodedAudio result;

    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    mp3dec_file_info_t info{};
    int ret = mp3dec_load_buf(&mp3d, data.data, data.size, &info, nullptr, nullptr);

    if (ret != 0 || !info.buffer || info.samples == 0) {
        result.error = "Failed to decode MP3 data";
        if (info.buffer) std::free(info.buffer);
        return result;
    }

    // Convert int16 interleaved to float
    result.samples.resize(info.samples);
    for (std::size_t i = 0; i < info.samples; ++i) {
        result.samples[i] = info.buffer[i] / 32768.0f;
    }

    result.sample_rate = static_cast<std::uint32_t>(info.hz);
    result.channels = static_cast<std::uint16_t>(info.channels);
    result.num_frames = static_cast<std::uint32_t>(
        static_cast<std::size_t>(info.samples) / static_cast<std::size_t>(info.channels));
    result.success = true;

    std::free(info.buffer);
    return result;
}

}  // namespace cedar

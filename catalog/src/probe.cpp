#include "catalog/probe.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

#include "cedar/io/audio_decoder.hpp"
#include "cedar/io/file_loader.hpp"

namespace catalog {

namespace {

std::string lower_ext(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::uint32_t read_u32le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t read_u16le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

ProbeResult fail(const char* msg) {
    ProbeResult r;
    r.error = msg;
    return r;
}

// Reads the head of a file — enough for WAV/FLAC header walks. The WAV chunk
// walk below never needs chunk *contents* beyond fmt (16-40 bytes), but a
// chunk directory can be preceded by large non-audio chunks, so we read a
// generous head and treat "fmt not found within it" as malformed.
constexpr std::size_t kHeadBytes = 256 * 1024;

std::vector<std::uint8_t> read_head(const std::string& path, std::size_t max_bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<std::uint8_t> buf(max_bytes);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(f.gcount()));
    return buf;
}

}  // namespace

ProbeResult probe_wav(const std::uint8_t* data, std::size_t size) {
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return fail("not a RIFF/WAVE file");
    }

    bool have_fmt = false;
    std::uint16_t format = 0, channels = 0, bits = 0, block_align = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t data_size = 0;
    bool have_data = false;

    std::size_t off = 12;
    while (off + 8 <= size) {
        const std::uint8_t* hdr = data + off;
        const std::uint32_t chunk_size = read_u32le(hdr + 4);
        const std::size_t body = off + 8;

        if (std::memcmp(hdr, "fmt ", 4) == 0) {
            if (chunk_size < 16 || body + 16 > size) return fail("truncated fmt chunk");
            const std::uint8_t* f = data + body;
            format = read_u16le(f);
            channels = read_u16le(f + 2);
            sample_rate = read_u32le(f + 4);
            block_align = read_u16le(f + 12);
            bits = read_u16le(f + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format tag lives in the first
            // two bytes of the SubFormat GUID; bit depth stays valid.
            if (format == 0xFFFE && chunk_size >= 40 && body + 26 <= size) {
                format = read_u16le(f + 24);
            }
            have_fmt = true;
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            data_size = chunk_size;
            have_data = true;
        }

        if (have_fmt && have_data) break;
        // Chunks are word-aligned; a zero-size chunk in a malformed file must
        // still advance.
        off = body + chunk_size + (chunk_size & 1);
        if (chunk_size == 0 && std::memcmp(hdr, "data", 4) != 0) return fail("malformed chunk list");
    }

    if (!have_fmt) return fail("no fmt chunk");
    if (!have_data) return fail("no data chunk");
    if (format != 1 && format != 3) return fail("unsupported WAV format tag");
    if (channels == 0 || sample_rate == 0 || bits == 0) return fail("invalid fmt fields");
    if (block_align == 0) block_align = static_cast<std::uint16_t>(channels * (bits / 8));
    if (block_align == 0) return fail("invalid block alignment");

    ProbeResult r;
    r.success = true;
    r.sample_rate = sample_rate;
    r.channels = channels;
    r.bit_depth = bits;
    r.num_frames = data_size / block_align;
    r.duration_s = static_cast<double>(r.num_frames) / static_cast<double>(sample_rate);
    return r;
}

ProbeResult probe_flac(const std::uint8_t* data, std::size_t size) {
    // fLaC magic + first metadata block must be STREAMINFO (34 bytes).
    if (size < 4 + 4 + 34 || std::memcmp(data, "fLaC", 4) != 0) return fail("not a FLAC file");
    const std::uint8_t* block = data + 4;
    if ((block[0] & 0x7F) != 0) return fail("first FLAC block is not STREAMINFO");
    const std::uint32_t len = (static_cast<std::uint32_t>(block[1]) << 16) |
                              (static_cast<std::uint32_t>(block[2]) << 8) | block[3];
    if (len < 34 || size < 8u + len) return fail("truncated STREAMINFO");

    const std::uint8_t* si = block + 4;
    // STREAMINFO layout from byte 10: 20 bits sample rate, 3 bits channels-1,
    // 5 bits bits-per-sample-1, 36 bits total samples.
    const std::uint32_t sample_rate = (static_cast<std::uint32_t>(si[10]) << 12) |
                                      (static_cast<std::uint32_t>(si[11]) << 4) | (si[12] >> 4);
    const std::uint16_t channels = static_cast<std::uint16_t>(((si[12] >> 1) & 0x07) + 1);
    const std::uint16_t bits =
        static_cast<std::uint16_t>((((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1);
    const std::uint64_t total_samples = (static_cast<std::uint64_t>(si[13] & 0x0F) << 32) |
                                        (static_cast<std::uint64_t>(si[14]) << 24) |
                                        (static_cast<std::uint64_t>(si[15]) << 16) |
                                        (static_cast<std::uint64_t>(si[16]) << 8) | si[17];

    if (sample_rate == 0) return fail("invalid FLAC sample rate");

    ProbeResult r;
    r.success = true;
    r.sample_rate = sample_rate;
    r.channels = channels;
    r.bit_depth = bits;
    r.num_frames = total_samples;  // 0 = unknown per spec; duration follows
    r.duration_s = static_cast<double>(total_samples) / static_cast<double>(sample_rate);
    return r;
}

ProbeResult probe_file(const std::string& path) {
    const std::string ext = lower_ext(path);

    if (ext == "wav" || ext == "wave") {
        const auto head = read_head(path, kHeadBytes);
        if (head.empty()) return fail("cannot read file");
        return probe_wav(head.data(), head.size());
    }
    if (ext == "flac") {
        const auto head = read_head(path, 64 * 1024);
        if (head.empty()) return fail("cannot read file");
        return probe_flac(head.data(), head.size());
    }
    if (ext == "ogg" || ext == "mp3") {
        auto loaded = cedar::FileLoader::load(path);
        if (!loaded.success()) return fail("cannot read file");
        const auto decoded = cedar::AudioDecoder::decode(loaded.buffer().view());
        if (!decoded.success) {
            ProbeResult r;
            r.error = decoded.error;
            return r;
        }
        ProbeResult r;
        r.success = true;
        r.sample_rate = decoded.sample_rate;
        r.channels = decoded.channels;
        r.num_frames = decoded.num_frames;
        r.duration_s =
            decoded.sample_rate ? static_cast<double>(decoded.num_frames) / decoded.sample_rate : 0.0;
        return r;
    }
    return fail("unsupported extension");
}

}  // namespace catalog

#include "catalog/corpus.hpp"

#include <array>
#include <cstdio>
#include <filesystem>

#include "catalog/wav_writer.hpp"

namespace catalog {

namespace fs = std::filesystem;

std::uint64_t generate_corpus(const std::string& dir, std::uint64_t count,
                              std::uint32_t files_per_dir) {
    static constexpr std::array<const char*, 8> kDescriptors = {
        "kick", "snare", "hat", "bass_loop", "guitar_riff", "vocal_chop", "pad", "perc"};
    static constexpr std::array<const char*, 6> kKeys = {"Amin", "Cmaj", "Dmin",
                                                         "Fsharpmin", "Gmaj", "Emin"};
    static constexpr std::array<int, 4> kBpms = {90, 120, 140, 174};

    if (files_per_dir == 0) files_per_dir = 1000;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return 0;

    // One tiny buffer reused for every file: 400 frames mono @ 8 kHz (~50 ms,
    // ~800 bytes of PCM) keeps a 100k corpus around ~100 MB.
    const auto pcm = sine_pcm16(400, 1, 440.0, 8000);

    std::uint64_t written = 0;
    char name[128];
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t pack = i / files_per_dir;
        if (i % files_per_dir == 0) {
            std::snprintf(name, sizeof(name), "pack_%04llu",
                          static_cast<unsigned long long>(pack));
            fs::create_directories(fs::path(dir) / name, ec);
            if (ec) return written;
        }
        std::snprintf(name, sizeof(name), "pack_%04llu/SC_%02llu_%d_%s_%06llu_%s.wav",
                      static_cast<unsigned long long>(pack),
                      static_cast<unsigned long long>(pack % 100),
                      kBpms[i % kBpms.size()], kDescriptors[i % kDescriptors.size()],
                      static_cast<unsigned long long>(i), kKeys[i % kKeys.size()]);
        if (!write_wav16((fs::path(dir) / name).string(), pcm, 8000, 1)) return written;
        ++written;
    }
    return written;
}

}  // namespace catalog

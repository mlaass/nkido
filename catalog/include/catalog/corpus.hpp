#pragma once

#include <cstdint>
#include <string>

namespace catalog {

// Synthetic test corpus: `count` small PCM16 WAVs spread over pack-style
// subdirectories, with realistic sample-library filenames (BPM tokens, key
// tokens, descriptors) so harvest/search phases can test against it too.
// Deterministic for a given (count, files_per_dir). Returns files written.
std::uint64_t generate_corpus(const std::string& dir, std::uint64_t count,
                              std::uint32_t files_per_dir = 1000);

}  // namespace catalog

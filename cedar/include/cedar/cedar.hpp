#pragma once

#include <cstdint>
#include <string_view>

namespace cedar {

// Forward declarations
class VM;

/// Cedar version information (injected via CMake compile definitions)
#ifndef CEDAR_VERSION_MAJOR
#define CEDAR_VERSION_MAJOR 0
#define CEDAR_VERSION_MINOR 0
#define CEDAR_VERSION_PATCH 0
#endif

#define CEDAR_STRINGIFY_(x) #x
#define CEDAR_STRINGIFY(x) CEDAR_STRINGIFY_(x)

struct Version {
    static constexpr int major = CEDAR_VERSION_MAJOR;
    static constexpr int minor = CEDAR_VERSION_MINOR;
    static constexpr int patch = CEDAR_VERSION_PATCH;

    static constexpr std::string_view string() {
        return CEDAR_STRINGIFY(CEDAR_VERSION_MAJOR) "."
               CEDAR_STRINGIFY(CEDAR_VERSION_MINOR) "."
               CEDAR_STRINGIFY(CEDAR_VERSION_PATCH);
    }
};

/// Default audio configuration
struct Config {
    std::uint32_t sample_rate = 48000;
    std::uint32_t block_size = 128;
    std::uint32_t channels = 2;
};

/// Initialize Cedar with the given configuration
/// Returns true on success
bool init(const Config& config = {});

/// Shutdown Cedar and release resources
void shutdown();

/// Get the current configuration
const Config& config();

} // namespace cedar

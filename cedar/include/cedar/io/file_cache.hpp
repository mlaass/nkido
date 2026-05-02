#pragma once

#ifndef __EMSCRIPTEN__

#include "cedar/io/buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cedar {

/// Disk-backed LRU cache for fetched assets (HTTP, github, etc.).
///
/// Storage layout: each entry is one file under the cache directory with
/// its name = hex(FNV-1a-64(uri)). Last-access tracking uses the file
/// mtime — reads `touch` it, evictions sort ascending by mtime.
///
/// The cache directory is platform-resolved at construction:
///   - Linux:   $XDG_CACHE_HOME/nkido/, falling back to ~/.cache/nkido/
///   - macOS:   ~/Library/Caches/nkido/
///   - Windows: %LOCALAPPDATA%/nkido/cache/
///
/// 500 MB cap; eviction runs on `set()` when adding the new entry would
/// exceed the cap.
///
/// Disk failures are non-fatal: corrupted or unreadable files are treated
/// as misses; failed writes log a warning and the caller proceeds with the
/// in-memory bytes (cache participation is opt-in middleware, not a
/// barrier).
class FileCache {
public:
    /// Maximum cumulative bytes-on-disk before eviction kicks in.
    static constexpr std::uintmax_t MAX_CACHE_SIZE = 500ull * 1024ull * 1024ull;

    /// Construct using the platform default cache directory.
    FileCache();

    /// Construct against a specific directory (for tests).
    explicit FileCache(std::filesystem::path cache_dir);

    /// Look up `uri` in the cache. Returns the bytes on hit, nullopt on
    /// miss or read failure. On hit, touches the file mtime so LRU order
    /// updates.
    [[nodiscard]] std::optional<OwnedBuffer> get(std::string_view uri);

    /// Store `bytes` for `uri`. Evicts least-recently-used entries first
    /// if needed. Silently no-ops on disk failure.
    void set(std::string_view uri, MemoryView bytes);

    /// Remove `uri`'s cache entry if present.
    void remove(std::string_view uri);

    /// Wipe all cache entries.
    void clear();

    /// Total bytes currently cached on disk.
    [[nodiscard]] std::uintmax_t total_size() const;

    /// The resolved cache directory.
    [[nodiscard]] const std::filesystem::path& directory() const { return dir_; }

private:
    std::filesystem::path dir_;

    [[nodiscard]] std::filesystem::path entry_path(std::string_view uri) const;
    void evict_to_fit(std::uintmax_t incoming_size);
};

/// Platform-default cache directory under "nkido". Created if missing.
[[nodiscard]] std::filesystem::path default_cache_directory();

}  // namespace cedar

#endif  // __EMSCRIPTEN__

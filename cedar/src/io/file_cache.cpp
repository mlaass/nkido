#ifndef __EMSCRIPTEN__

#include "cedar/io/file_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

namespace cedar {

namespace {

/// FNV-1a 64-bit hash; deterministic across runs and platforms, plenty
/// large enough that collisions on a few-thousand-entry cache are
/// negligible.
std::uint64_t fnv1a_64(std::string_view s) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string to_hex_64(std::uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(buf);
}

#if defined(_WIN32)
std::filesystem::path resolve_default_cache_dir() {
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
        return std::filesystem::path(local) / "nkido" / "cache";
    }
    return std::filesystem::temp_directory_path() / "nkido-cache";
}
#elif defined(__APPLE__)
std::filesystem::path resolve_default_cache_dir() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Caches" / "nkido";
    }
    return std::filesystem::temp_directory_path() / "nkido-cache";
}
#else
std::filesystem::path resolve_default_cache_dir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "nkido";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".cache" / "nkido";
    }
    return std::filesystem::temp_directory_path() / "nkido-cache";
}
#endif

void touch_mtime(const std::filesystem::path& path) {
    std::error_code ec;
    auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(path, now, ec);
    // Best-effort; ignore failure.
}

}  // namespace

std::filesystem::path default_cache_directory() {
    auto dir = resolve_default_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

FileCache::FileCache() : FileCache(default_cache_directory()) {}

FileCache::FileCache(std::filesystem::path cache_dir) : dir_(std::move(cache_dir)) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
}

std::filesystem::path FileCache::entry_path(std::string_view uri) const {
    return dir_ / to_hex_64(fnv1a_64(uri));
}

std::optional<OwnedBuffer> FileCache::get(std::string_view uri) {
    auto path = entry_path(uri);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    auto sz = std::filesystem::file_size(path, ec);
    if (ec) return std::nullopt;

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
    if (sz > 0) {
        in.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(sz));
        if (!in) return std::nullopt;  // Treat as miss; corrupt entry.
    }

    touch_mtime(path);
    return OwnedBuffer(std::move(bytes));
}

void FileCache::set(std::string_view uri, MemoryView bytes) {
    if (bytes.size > MAX_CACHE_SIZE) {
        // Single asset is too big to ever fit. Don't bother caching.
        return;
    }

    evict_to_fit(bytes.size);

    auto path = entry_path(uri);
    auto tmp = path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        if (bytes.size > 0) {
            out.write(reinterpret_cast<const char*>(bytes.data),
                      static_cast<std::streamsize>(bytes.size));
        }
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return;
    }
    touch_mtime(path);
}

void FileCache::remove(std::string_view uri) {
    std::error_code ec;
    std::filesystem::remove(entry_path(uri), ec);
}

void FileCache::clear() {
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        std::filesystem::remove(entry.path(), ec);
    }
}

std::uintmax_t FileCache::total_size() const {
    std::uintmax_t total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) return 0;
    for (auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

void FileCache::evict_to_fit(std::uintmax_t incoming_size) {
    auto current = total_size();
    if (current + incoming_size <= MAX_CACHE_SIZE) return;

    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir_, ec)) {
        if (!e.is_regular_file(ec)) continue;
        Entry record;
        record.path = e.path();
        record.mtime = e.last_write_time(ec);
        record.size = e.file_size(ec);
        entries.push_back(std::move(record));
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });

    for (auto& e : entries) {
        if (current + incoming_size <= MAX_CACHE_SIZE) break;
        std::filesystem::remove(e.path, ec);
        if (!ec) current -= e.size;
    }
}

}  // namespace cedar

#endif  // __EMSCRIPTEN__

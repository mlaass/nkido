#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace catalog::test {

// Unique per-instance temp directory, removed on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("catalog_test_" + std::to_string(tick) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::string str(const std::string& rel = "") const {
        return rel.empty() ? path_.string() : (path_ / rel).string();
    }

private:
    std::filesystem::path path_;
};

inline void write_bytes(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

inline void write_text(const std::string& path, const std::string& text) {
    write_bytes(path, text.data(), text.size());
}

}  // namespace catalog::test

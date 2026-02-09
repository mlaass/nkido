#ifndef __EMSCRIPTEN__

#include "cedar/io/file_loader.hpp"

#include <fstream>

namespace cedar {

LoadResult FileLoader::load(const std::string& path) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return {FileLoadError{FileError::NotFound, "File not found: " + path}};
    }

    auto fsize = fs::file_size(path, ec);
    if (ec) {
        return {FileLoadError{FileError::PermissionDenied, "Cannot read file size: " + path}};
    }

    // Reject files larger than 256MB
    constexpr std::uintmax_t max_size = 256 * 1024 * 1024;
    if (fsize > max_size) {
        return {FileLoadError{FileError::TooLarge, "File too large: " + std::to_string(fsize) + " bytes"}};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {FileLoadError{FileError::PermissionDenied, "Cannot open file: " + path}};
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(fsize));
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fsize));

    if (!file) {
        return {FileLoadError{FileError::Corrupted, "Failed to read file: " + path}};
    }

    return {OwnedBuffer(std::move(buffer))};
}

bool FileLoader::exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::optional<std::size_t> FileLoader::file_size(const std::string& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) return std::nullopt;
    return static_cast<std::size_t>(size);
}

}  // namespace cedar

#endif  // __EMSCRIPTEN__

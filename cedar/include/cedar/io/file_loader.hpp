#pragma once

#ifndef __EMSCRIPTEN__

#include "cedar/io/buffer.hpp"
#include "cedar/io/errors.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace cedar {

struct LoadResult {
    std::variant<OwnedBuffer, FileLoadError> value;

    [[nodiscard]] bool success() const {
        return std::holds_alternative<OwnedBuffer>(value);
    }

    [[nodiscard]] OwnedBuffer& buffer() {
        return std::get<OwnedBuffer>(value);
    }

    [[nodiscard]] const OwnedBuffer& buffer() const {
        return std::get<OwnedBuffer>(value);
    }

    [[nodiscard]] const FileLoadError& error() const {
        return std::get<FileLoadError>(value);
    }
};

class FileLoader {
public:
    /// Load entire file into memory
    static LoadResult load(const std::string& path);

    /// Check if file exists
    static bool exists(const std::string& path);

    /// Get file size in bytes
    static std::optional<std::size_t> file_size(const std::string& path);
};

}  // namespace cedar

#endif  // __EMSCRIPTEN__

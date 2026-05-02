#pragma once

#include <string>

namespace cedar {

enum class FileError {
    NotFound,
    PermissionDenied,
    TooLarge,
    InvalidFormat,
    Corrupted,
    UnsupportedFormat,
    NetworkError,
    Aborted
};

struct FileLoadError {
    FileError code;
    std::string message;
};

}  // namespace cedar

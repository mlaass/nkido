#ifndef __EMSCRIPTEN__

#include "cedar/io/handlers/file_handler.hpp"

#include "cedar/io/file_loader.hpp"

#include <string>

namespace cedar {

namespace {

/// Strip a "file://" or "file:" prefix if present, returning the path.
/// Otherwise return uri unchanged (bare-path case).
std::string_view strip_file_scheme(std::string_view uri) {
    constexpr std::string_view triple = "file://";
    constexpr std::string_view single = "file:";
    if (uri.substr(0, triple.size()) == triple) {
        return uri.substr(triple.size());
    }
    if (uri.substr(0, single.size()) == single) {
        return uri.substr(single.size());
    }
    return uri;
}

}  // namespace

LoadResult FileHandler::load(std::string_view uri) const {
    std::string path(strip_file_scheme(uri));
    return FileLoader::load(path);
}

}  // namespace cedar

#endif  // __EMSCRIPTEN__

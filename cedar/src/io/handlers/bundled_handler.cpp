#include "cedar/io/handlers/bundled_handler.hpp"

#include <string>

namespace cedar {

namespace {

/// Strip "bundled://" or "bundled:" prefix if present.
std::string_view strip_bundled_scheme(std::string_view uri) {
    constexpr std::string_view triple = "bundled://";
    constexpr std::string_view single = "bundled:";
    if (uri.substr(0, triple.size()) == triple) {
        return uri.substr(triple.size());
    }
    if (uri.substr(0, single.size()) == single) {
        return uri.substr(single.size());
    }
    return uri;
}

}  // namespace

LoadResult BundledHandler::load(std::string_view uri) const {
    std::string name(strip_bundled_scheme(uri));
    auto it = assets_.find(name);
    if (it == assets_.end()) {
        return {FileLoadError{FileError::NotFound,
                              "bundled asset not found: '" + name + "'"}};
    }
    return {OwnedBuffer(std::vector<std::uint8_t>(it->second))};
}

void BundledHandler::register_asset(std::string name, std::vector<std::uint8_t> bytes) {
    assets_[std::move(name)] = std::move(bytes);
}

void BundledHandler::clear() {
    assets_.clear();
}

}  // namespace cedar

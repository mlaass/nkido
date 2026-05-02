#include "cedar/io/uri_resolver.hpp"

#include <cctype>
#include <string>

namespace cedar {

namespace {

bool is_windows_drive_letter(std::string_view uri) {
    return uri.size() >= 2
        && std::isalpha(static_cast<unsigned char>(uri[0]))
        && uri[1] == ':';
}

bool is_bare_path(std::string_view uri) {
    if (uri.empty()) return false;
    if (uri[0] == '/' || uri[0] == '.') return true;
    if (is_windows_drive_letter(uri)) return true;
    return false;
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

std::string_view UriResolver::extract_scheme(std::string_view uri) {
    if (is_bare_path(uri)) return {};
    auto colon = uri.find(':');
    if (colon == std::string_view::npos) return {};
    return uri.substr(0, colon);
}

UriResolver& UriResolver::instance() {
    static UriResolver resolver;
    return resolver;
}

void UriResolver::register_handler(std::unique_ptr<UriHandler> handler) {
    if (!handler) return;
    std::string key = to_lower(handler->scheme());
    handlers_[std::move(key)] = std::move(handler);
}

const UriHandler* UriResolver::handler_for(std::string_view scheme) const {
    auto it = handlers_.find(to_lower(scheme));
    return it == handlers_.end() ? nullptr : it->second.get();
}

LoadResult UriResolver::load(std::string_view uri) const {
    if (uri.empty()) {
        return {FileLoadError{FileError::InvalidFormat, "empty URI"}};
    }

    std::string_view scheme = extract_scheme(uri);
    if (scheme.empty()) {
        // Bare path — treat as file://
        scheme = "file";
    }

    const UriHandler* h = handler_for(scheme);
    if (!h) {
        return {FileLoadError{FileError::UnsupportedFormat,
                              "no handler registered for scheme '" + std::string(scheme) + "'"}};
    }

    return h->load(uri);
}

}  // namespace cedar

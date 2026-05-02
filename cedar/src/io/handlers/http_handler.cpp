#ifndef __EMSCRIPTEN__

#include "cedar/io/handlers/http_handler.hpp"

#include "cedar/io/file_cache.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <string>
#include <utility>
#include <vector>

namespace cedar {

namespace {

struct ParsedUrl {
    std::string scheme;   // "http" or "https"
    std::string host;
    int port = -1;        // -1 = default for scheme
    std::string path;     // includes leading '/' and any ?query / #fragment
};

/// Parse an absolute http(s) URL into scheme/host/port/path. Returns
/// nullopt on malformed input.
std::optional<ParsedUrl> parse_url(std::string_view url) {
    auto scheme_sep = url.find("://");
    if (scheme_sep == std::string_view::npos) return std::nullopt;

    ParsedUrl out;
    out.scheme = std::string(url.substr(0, scheme_sep));

    std::string_view rest = url.substr(scheme_sep + 3);
    auto path_start = rest.find('/');
    std::string_view authority = path_start == std::string_view::npos
                                     ? rest
                                     : rest.substr(0, path_start);
    out.path = path_start == std::string_view::npos
                   ? "/"
                   : std::string(rest.substr(path_start));

    auto port_sep = authority.find(':');
    if (port_sep == std::string_view::npos) {
        out.host = std::string(authority);
    } else {
        out.host = std::string(authority.substr(0, port_sep));
        try {
            out.port = std::stoi(std::string(authority.substr(port_sep + 1)));
        } catch (...) {
            return std::nullopt;
        }
    }
    if (out.host.empty()) return std::nullopt;
    return out;
}

LoadResult make_error(FileError code, std::string msg) {
    return {FileLoadError{code, std::move(msg)}};
}

}  // namespace

HttpHandler::HttpHandler(std::string scheme, FileCache* cache)
    : scheme_(std::move(scheme)), cache_(cache) {}

LoadResult HttpHandler::load(std::string_view uri) const {
    // Cache fast-path: serve from disk before touching the network.
    if (cache_) {
        if (auto hit = cache_->get(uri); hit.has_value()) {
            return {std::move(*hit)};
        }
    }

    auto parsed = parse_url(uri);
    if (!parsed) {
        return make_error(FileError::InvalidFormat,
                          "invalid HTTP URL: " + std::string(uri));
    }

    httplib::Client cli(parsed->scheme + "://" + parsed->host
                        + (parsed->port > 0 ? ":" + std::to_string(parsed->port) : ""));
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);
    cli.set_write_timeout(timeout_);
    cli.set_follow_location(max_redirects_ > 0);
    cli.enable_server_certificate_verification(true);

    auto result = cli.Get(parsed->path);
    if (!result) {
        return make_error(FileError::NetworkError,
                          "HTTP request failed: "
                              + httplib::to_string(result.error())
                              + " (" + std::string(uri) + ")");
    }

    int status = result->status;
    if (status == 404) {
        return make_error(FileError::NotFound,
                          "HTTP 404: " + std::string(uri));
    }
    if (status < 200 || status >= 300) {
        return make_error(FileError::NetworkError,
                          "HTTP " + std::to_string(status) + ": "
                              + std::string(uri));
    }

    std::vector<std::uint8_t> bytes(result->body.begin(), result->body.end());
    OwnedBuffer owned(std::move(bytes));

    if (cache_) {
        cache_->set(uri, owned.view());
    }

    return {std::move(owned)};
}

}  // namespace cedar

#endif  // __EMSCRIPTEN__

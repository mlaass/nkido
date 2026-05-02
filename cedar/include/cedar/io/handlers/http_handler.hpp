#pragma once

#ifndef __EMSCRIPTEN__

#include "cedar/io/uri_resolver.hpp"

#include <chrono>
#include <string>

namespace cedar {

class FileCache;

/// http:// and https:// scheme handler. Uses cpp-httplib (vendored) for
/// the underlying client. HTTPS routes through OpenSSL via the system
/// certificate store.
///
/// One handler instance covers both schemes; register it twice (once for
/// "http", once for "https") at host startup.
///
/// Optional `cache` pointer enables transparent disk caching via
/// `FileCache`. The cache is keyed by the canonical URI string (the
/// `uri` argument passed to load()), so callers benefit from sharing
/// cache entries across schemes that recurse through this handler (e.g.
/// the github handler resolves to https:// URIs that share the cache).
class HttpHandler final : public UriHandler {
public:
    /// Construct for `scheme` ("http" or "https"). `cache` is optional;
    /// pass nullptr to disable caching.
    explicit HttpHandler(std::string scheme, FileCache* cache = nullptr);

    /// Set the request timeout (default: 30 seconds).
    void set_timeout(std::chrono::seconds timeout) { timeout_ = timeout; }

    /// Set the maximum number of redirects to follow (default: 5).
    void set_max_redirects(int n) { max_redirects_ = n; }

    [[nodiscard]] std::string_view scheme() const override { return scheme_; }
    [[nodiscard]] LoadResult load(std::string_view uri) const override;

private:
    std::string scheme_;
    FileCache* cache_;
    std::chrono::seconds timeout_{30};
    int max_redirects_{5};
};

}  // namespace cedar

#endif  // __EMSCRIPTEN__

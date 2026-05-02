#pragma once

#include "cedar/io/file_loader.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cedar {

/// One scheme handler. Subclass per scheme; register on UriResolver.
class UriHandler {
public:
    virtual ~UriHandler() = default;

    /// Lowercase scheme identifier without trailing punctuation
    /// (e.g. "file", "https", "github", "bundled").
    [[nodiscard]] virtual std::string_view scheme() const = 0;

    /// Resolve `uri` (full URI including scheme) to bytes. Synchronous.
    /// Hosts running on a background thread may call this directly.
    [[nodiscard]] virtual LoadResult load(std::string_view uri) const = 0;
};

/// Scheme-keyed dispatcher for asset loading.
///
/// Hosts populate the singleton at startup with the handlers appropriate to
/// their environment (e.g. native CLI registers file/http/github/bundled;
/// WASM registers only bundled because TS handles the rest before calling
/// into WASM).
///
/// Bare paths (no scheme) are treated as file:// — a Windows drive letter
/// like "C:\foo" is detected and routed to file:// rather than parsed as a
/// scheme called "C".
class UriResolver {
public:
    /// Process-global singleton. Hosts populate it at startup.
    static UriResolver& instance();

    /// Register a handler for its declared scheme. Last registration wins.
    void register_handler(std::unique_ptr<UriHandler> handler);

    /// Look up handler by scheme. Returns nullptr if no handler registered.
    [[nodiscard]] const UriHandler* handler_for(std::string_view scheme) const;

    /// Parse scheme from `uri`, dispatch to its handler, return bytes.
    /// Bare paths are routed to the "file" handler.
    /// Returns FileError::UnsupportedFormat if no handler is registered for
    /// the scheme; FileError::InvalidFormat if `uri` is empty.
    [[nodiscard]] LoadResult load(std::string_view uri) const;

    /// Extract the lowercase scheme from `uri`, or empty string if `uri` is
    /// a bare path (no scheme separator, or a Windows drive letter).
    [[nodiscard]] static std::string_view extract_scheme(std::string_view uri);

private:
    UriResolver() = default;
    UriResolver(const UriResolver&) = delete;
    UriResolver& operator=(const UriResolver&) = delete;

    std::unordered_map<std::string, std::unique_ptr<UriHandler>> handlers_;
};

}  // namespace cedar

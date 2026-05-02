#pragma once

#include "cedar/io/uri_resolver.hpp"

#include <string>

namespace cedar {

/// github: scheme handler. Transforms `github:user/repo[/branch][/path]`
/// into a `https://raw.githubusercontent.com/...` URL and recurses
/// through the resolver. Always built — works wherever an https handler
/// is registered.
///
/// Heuristic: if the path component ends in a known audio extension
/// (.wav, .ogg, .flac, .mp3, .sf2, .sf3, .json), fetch as-is. Otherwise
/// treat as a directory containing `strudel.json`.
///
/// Default branch: "main".
class GithubHandler final : public UriHandler {
public:
    [[nodiscard]] std::string_view scheme() const override { return "github"; }
    [[nodiscard]] LoadResult load(std::string_view uri) const override;

    /// Transform a `github:` URI into the raw.githubusercontent.com URL
    /// it resolves to. Exposed for testing.
    [[nodiscard]] static std::string to_https_url(std::string_view uri);
};

}  // namespace cedar

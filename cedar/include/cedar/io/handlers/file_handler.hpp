#pragma once

#ifndef __EMSCRIPTEN__

#include "cedar/io/uri_resolver.hpp"

namespace cedar {

/// file:// scheme handler. Native only; WASM does not register this.
/// Accepts:
///   - "file:///abs/path"           (URL form)
///   - "/abs/path", "./rel", "C:\path"  (bare paths, routed via UriResolver)
class FileHandler final : public UriHandler {
public:
    [[nodiscard]] std::string_view scheme() const override { return "file"; }
    [[nodiscard]] LoadResult load(std::string_view uri) const override;
};

}  // namespace cedar

#endif  // __EMSCRIPTEN__

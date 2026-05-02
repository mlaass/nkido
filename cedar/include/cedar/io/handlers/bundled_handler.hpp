#pragma once

#include "cedar/io/uri_resolver.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cedar {

/// bundled:// (or bundled:) scheme handler. Looks up the asset name in an
/// in-process table populated at startup by the host. The table is
/// populated externally (e.g. via a linker-generated symbol or a manifest
/// loaded by the host) — this handler only does name → bytes lookup.
///
/// Always available on both native and WASM. Empty by default.
class BundledHandler final : public UriHandler {
public:
    [[nodiscard]] std::string_view scheme() const override { return "bundled"; }
    [[nodiscard]] LoadResult load(std::string_view uri) const override;

    /// Register a bundled asset under `name` with `bytes`. Last
    /// registration for a name wins.
    void register_asset(std::string name, std::vector<std::uint8_t> bytes);

    /// Remove all registered assets (mostly for tests).
    void clear();

private:
    std::unordered_map<std::string, std::vector<std::uint8_t>> assets_;
};

}  // namespace cedar

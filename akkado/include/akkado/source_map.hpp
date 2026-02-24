#pragma once

#include "diagnostics.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace akkado {

// Forward declarations to avoid heavy includes
struct StateInitData;
struct VisualizationDecl;

/// Maps byte offsets and line numbers in a concatenated source string
/// back to their original files. Generalizes the old two-region
/// (stdlib + user) adjustment to N regions.
class SourceMap {
public:
    struct Region {
        std::string filename;
        std::size_t byte_offset;    // Start offset in combined source
        std::size_t byte_length;    // Length of this region in bytes
        std::size_t line_offset;    // Cumulative line count before this region
    };

    /// Add a region. Must be called in byte-offset order.
    void add_region(std::string filename, std::size_t byte_offset,
                    std::size_t byte_length, std::size_t line_offset);

    /// Adjust all diagnostics (primary + related + fix locations)
    void adjust_all(std::vector<Diagnostic>& diagnostics) const;

    /// Adjust source_locations parallel to bytecode instructions
    void adjust_source_locations(std::vector<SourceLocation>& locations) const;

    /// Adjust StateInitData pattern locations
    void adjust_state_inits(std::vector<StateInitData>& inits) const;

    /// Adjust VisualizationDecl source offsets
    void adjust_viz_decls(std::vector<VisualizationDecl>& decls) const;

    /// Find region containing a byte offset. Returns nullptr if not found.
    const Region* find_region(std::size_t byte_offset) const;

    const std::vector<Region>& regions() const { return regions_; }

private:
    void adjust_location(SourceLocation& loc, std::string& filename) const;

    std::vector<Region> regions_;
};

} // namespace akkado

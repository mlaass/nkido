#include "akkado/source_map.hpp"
#include "akkado/codegen.hpp"  // StateInitData, VisualizationDecl

namespace akkado {

void SourceMap::add_region(std::string filename, std::size_t byte_offset,
                           std::size_t byte_length, std::size_t line_offset) {
    regions_.push_back(Region{
        .filename = std::move(filename),
        .byte_offset = byte_offset,
        .byte_length = byte_length,
        .line_offset = line_offset
    });
}

const SourceMap::Region* SourceMap::find_region(std::size_t byte_offset) const {
    // Regions are in byte-offset order. Walk backwards to find the last region
    // whose byte_offset <= the target. This is correct because regions don't overlap.
    const Region* found = nullptr;
    for (const auto& r : regions_) {
        if (byte_offset >= r.byte_offset && byte_offset < r.byte_offset + r.byte_length) {
            found = &r;
            break;
        }
    }
    return found;
}

void SourceMap::adjust_location(SourceLocation& loc, std::string& filename) const {
    const Region* region = find_region(loc.offset);
    if (!region) return;

    loc.line -= static_cast<std::uint32_t>(region->line_offset);
    loc.offset -= static_cast<std::uint32_t>(region->byte_offset);
    filename = region->filename;
}

void SourceMap::adjust_all(std::vector<Diagnostic>& diagnostics) const {
    for (auto& diag : diagnostics) {
        adjust_location(diag.location, diag.filename);

        for (auto& rel : diag.related) {
            adjust_location(rel.location, rel.filename);
        }

        if (diag.fix) {
            // Fix doesn't have its own filename field; adjust offset/line only
            // using the region determined by offset
            const Region* region = find_region(diag.fix->location.offset);
            if (region) {
                diag.fix->location.line -= static_cast<std::uint32_t>(region->line_offset);
                diag.fix->location.offset -= static_cast<std::uint32_t>(region->byte_offset);
            }
        }
    }
}

void SourceMap::adjust_source_locations(std::vector<SourceLocation>& locations) const {
    for (auto& loc : locations) {
        const Region* region = find_region(loc.offset);
        if (region) {
            loc.offset -= static_cast<std::uint32_t>(region->byte_offset);
            loc.line -= static_cast<std::uint32_t>(region->line_offset);
        }
    }
}

void SourceMap::adjust_state_inits(std::vector<StateInitData>& inits) const {
    for (auto& init : inits) {
        const Region* region = find_region(init.pattern_location.offset);
        if (region) {
            init.pattern_location.offset -= static_cast<std::uint32_t>(region->byte_offset);
        }
    }
}

void SourceMap::adjust_viz_decls(std::vector<VisualizationDecl>& decls) const {
    for (auto& viz : decls) {
        const Region* region = find_region(viz.source_offset);
        if (region) {
            viz.source_offset -= static_cast<std::uint32_t>(region->byte_offset);
        }
    }
}

} // namespace akkado

#pragma once

#include "diagnostics.hpp"
#include "file_resolver.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace akkado {

/// A single import directive found by the scanner
struct ImportDirective {
    std::string path;             // The import path string
    std::string alias;            // Empty for direct injection; "as X" sets this to "X"
    std::size_t line_number;      // 1-based line number in the source
    std::size_t line_start;       // Byte offset of the line start
    std::size_t line_length;      // Byte length of the line (including newline)
};

/// A module that has been resolved and read
struct ResolvedModule {
    std::string canonical_path;   // Unique identifier (canonical file path or virtual key)
    std::string source;           // File contents with import lines blanked
};

/// Result of import resolution
struct ImportResult {
    /// Dependency modules in topological order (dependencies first).
    /// Does NOT include the root module.
    std::vector<ResolvedModule> modules;

    /// Root source with import lines blanked (preserved byte offsets)
    std::string root_source;

    std::vector<Diagnostic> diagnostics;
    bool success = true;
};

/// Extract import directives from source using lightweight line-based matching.
/// Stops scanning at the first non-import, non-comment, non-blank line.
/// Exported for testing.
std::vector<ImportDirective> scan_imports(std::string_view source);

/// Recursively resolve all imports from the root source.
/// Returns modules in topological order with blanked import lines.
ImportResult resolve_imports(
    std::string_view source,
    std::string_view filename,
    const FileResolver& resolver);

} // namespace akkado

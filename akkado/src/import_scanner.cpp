#include "akkado/import_scanner.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace akkado {

// ============================================================================
// scan_imports — lightweight line-based pre-parser
// ============================================================================

/// Skip whitespace, return iterator past it
static std::string_view ltrim(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

/// Parse a quoted string starting at s[0] == '"'. Returns content and advances past closing quote.
/// On failure returns nullopt.
static std::optional<std::string> parse_quoted_string(std::string_view s, std::size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') return std::nullopt;
    ++pos;  // skip opening quote
    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        result.push_back(s[pos]);
        ++pos;
    }
    if (pos >= s.size()) return std::nullopt;  // unterminated
    ++pos;  // skip closing quote
    return result;
}

/// Check if a character is valid for an identifier start
static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

std::vector<ImportDirective> scan_imports(std::string_view source) {
    std::vector<ImportDirective> directives;

    std::size_t offset = 0;
    std::size_t line_num = 1;

    while (offset < source.size()) {
        // Find end of current line
        std::size_t line_start = offset;
        std::size_t eol = source.find('\n', offset);
        std::size_t line_end = (eol == std::string_view::npos) ? source.size() : eol;
        std::size_t line_length = (eol == std::string_view::npos) ? (source.size() - line_start) : (eol - line_start + 1);  // include \n

        std::string_view line = source.substr(line_start, line_end - line_start);
        std::string_view trimmed = ltrim(line);

        // Skip blank lines
        if (trimmed.empty()) {
            offset = line_start + line_length;
            ++line_num;
            continue;
        }

        // Skip comment lines
        if (trimmed.starts_with("//")) {
            offset = line_start + line_length;
            ++line_num;
            continue;
        }

        // Check for "import" keyword
        if (!trimmed.starts_with("import") ||
            (trimmed.size() > 6 && is_ident_char(trimmed[6]))) {
            // Not an import — stop scanning (imports must be at top)
            break;
        }

        // Parse: import "path" [as alias]
        std::string_view rest = ltrim(trimmed.substr(6));

        // Parse the path string
        std::size_t pos = 0;
        auto path = parse_quoted_string(rest, pos);
        if (!path) {
            // Malformed import — stop scanning, let the parser report the error
            break;
        }

        // Optional: as <ident>
        std::string alias;
        std::string_view after_path = ltrim(rest.substr(pos));
        if (after_path.starts_with("as") &&
            (after_path.size() <= 2 || !is_ident_char(after_path[2]))) {
            std::string_view after_as = ltrim(after_path.substr(2));
            // Parse identifier
            std::size_t ident_len = 0;
            if (!after_as.empty() && is_ident_start(after_as[0])) {
                ident_len = 1;
                while (ident_len < after_as.size() && is_ident_char(after_as[ident_len])) {
                    ++ident_len;
                }
                alias = std::string(after_as.substr(0, ident_len));
            }
        }

        directives.push_back(ImportDirective{
            .path = std::move(*path),
            .alias = std::move(alias),
            .line_number = line_num,
            .line_start = line_start,
            .line_length = line_length
        });

        offset = line_start + line_length;
        ++line_num;
    }

    return directives;
}

// ============================================================================
// Blank import lines — replace non-newline chars with spaces
// ============================================================================

static std::string blank_imports(std::string_view source, const std::vector<ImportDirective>& directives) {
    std::string result(source);
    for (const auto& dir : directives) {
        for (std::size_t i = dir.line_start; i < dir.line_start + dir.line_length && i < result.size(); ++i) {
            if (result[i] != '\n') {
                result[i] = ' ';
            }
        }
    }
    return result;
}

// ============================================================================
// resolve_imports — recursive resolution with cycle detection + topo sort
// ============================================================================

namespace {

struct ModuleNode {
    std::string canonical_path;
    std::string source;                    // with import lines blanked
    std::vector<std::string> dependencies; // canonical paths of imports
};

enum class Mark { None, InProgress, Done };

/// DFS post-order visit for topological sort + cycle detection
bool topo_visit(
    const std::string& path,
    const std::unordered_map<std::string, ModuleNode>& graph,
    std::unordered_map<std::string, Mark>& marks,
    std::vector<std::string>& order,
    std::vector<std::string>& cycle_stack,
    std::vector<Diagnostic>& diagnostics,
    std::string_view root_filename) {

    auto& mark = marks[path];
    if (mark == Mark::Done) return true;

    if (mark == Mark::InProgress) {
        // Cycle detected — build the cycle path string
        std::string cycle_path;
        bool in_cycle = false;
        for (const auto& s : cycle_stack) {
            if (s == path) in_cycle = true;
            if (in_cycle) {
                if (!cycle_path.empty()) cycle_path += " -> ";
                cycle_path += s;
            }
        }
        cycle_path += " -> " + path;

        diagnostics.push_back(Diagnostic{
            .severity = Severity::Error,
            .code = "E500",
            .message = "Circular import detected: " + cycle_path,
            .filename = std::string(root_filename),
            .location = {}
        });
        return false;
    }

    mark = Mark::InProgress;
    cycle_stack.push_back(path);

    auto it = graph.find(path);
    if (it != graph.end()) {
        for (const auto& dep : it->second.dependencies) {
            if (!topo_visit(dep, graph, marks, order, cycle_stack, diagnostics, root_filename)) {
                return false;
            }
        }
    }

    cycle_stack.pop_back();
    mark = Mark::Done;
    order.push_back(path);
    return true;
}

} // anonymous namespace

ImportResult resolve_imports(
    std::string_view source,
    std::string_view filename,
    const FileResolver& resolver) {

    ImportResult result;

    // Scan root source for imports
    auto root_directives = scan_imports(source);
    if (root_directives.empty()) {
        // No imports — return blanked root (which is identical to source)
        result.root_source = std::string(source);
        return result;
    }

    // Build the module graph via BFS
    std::unordered_map<std::string, ModuleNode> graph;
    std::unordered_set<std::string> visited;

    struct WorkItem {
        std::string canonical_path;
        std::string source;
        std::string from_file;
        std::vector<ImportDirective> directives;
    };

    std::vector<WorkItem> worklist;

    // Process root imports
    std::string root_canonical(filename);
    std::vector<std::string> root_deps;

    for (const auto& dir : root_directives) {
        auto resolved = resolver.resolve(dir.path, filename);
        if (!resolved) {
            result.diagnostics.push_back(Diagnostic{
                .severity = Severity::Error,
                .code = "E502",
                .message = "Module not found: '" + dir.path + "'",
                .filename = std::string(filename),
                .location = {
                    .line = static_cast<std::uint32_t>(dir.line_number),
                    .column = 1,
                    .offset = static_cast<std::uint32_t>(dir.line_start),
                    .length = static_cast<std::uint32_t>(dir.line_length)
                }
            });
            result.success = false;
            continue;
        }

        root_deps.push_back(*resolved);

        // Track namespace imports (root-level only)
        if (!dir.alias.empty()) {
            result.namespaced_imports.push_back(NamespacedImport{
                .canonical_path = *resolved,
                .alias = dir.alias
            });
        }

        if (!visited.contains(*resolved)) {
            visited.insert(*resolved);
            auto module_source = resolver.read(*resolved);
            if (!module_source) {
                result.diagnostics.push_back(Diagnostic{
                    .severity = Severity::Error,
                    .code = "E503",
                    .message = "Failed to read module: '" + *resolved + "'",
                    .filename = std::string(filename),
                    .location = {
                        .line = static_cast<std::uint32_t>(dir.line_number),
                        .column = 1,
                        .offset = static_cast<std::uint32_t>(dir.line_start),
                        .length = static_cast<std::uint32_t>(dir.line_length)
                    }
                });
                result.success = false;
                continue;
            }

            auto mod_directives = scan_imports(*module_source);
            worklist.push_back(WorkItem{
                .canonical_path = *resolved,
                .source = std::move(*module_source),
                .from_file = *resolved,
                .directives = std::move(mod_directives)
            });
        }
    }

    if (!result.success) return result;

    // BFS: process transitive imports
    while (!worklist.empty()) {
        auto item = std::move(worklist.back());
        worklist.pop_back();

        std::vector<std::string> deps;

        for (const auto& dir : item.directives) {
            auto resolved = resolver.resolve(dir.path, item.from_file);
            if (!resolved) {
                result.diagnostics.push_back(Diagnostic{
                    .severity = Severity::Error,
                    .code = "E502",
                    .message = "Module not found: '" + dir.path + "'",
                    .filename = item.canonical_path,
                    .location = {
                        .line = static_cast<std::uint32_t>(dir.line_number),
                        .column = 1,
                        .offset = static_cast<std::uint32_t>(dir.line_start),
                        .length = static_cast<std::uint32_t>(dir.line_length)
                    }
                });
                result.success = false;
                continue;
            }

            deps.push_back(*resolved);

            if (!visited.contains(*resolved)) {
                visited.insert(*resolved);
                auto module_source = resolver.read(*resolved);
                if (!module_source) {
                    result.diagnostics.push_back(Diagnostic{
                        .severity = Severity::Error,
                        .code = "E503",
                        .message = "Failed to read module: '" + *resolved + "'",
                        .filename = item.canonical_path,
                        .location = {
                            .line = static_cast<std::uint32_t>(dir.line_number),
                            .column = 1,
                            .offset = static_cast<std::uint32_t>(dir.line_start),
                            .length = static_cast<std::uint32_t>(dir.line_length)
                        }
                    });
                    result.success = false;
                    continue;
                }

                auto mod_directives = scan_imports(*module_source);
                worklist.push_back(WorkItem{
                    .canonical_path = *resolved,
                    .source = std::move(*module_source),
                    .from_file = *resolved,
                    .directives = std::move(mod_directives)
                });
            }
        }

        if (!result.success) return result;

        // Blank import lines and store
        graph[item.canonical_path] = ModuleNode{
            .canonical_path = item.canonical_path,
            .source = blank_imports(item.source, item.directives),
            .dependencies = std::move(deps)
        };
    }

    // Also add root to graph (for topo sort), but we won't include it in output
    graph[root_canonical] = ModuleNode{
        .canonical_path = root_canonical,
        .source = {},  // root source handled separately
        .dependencies = std::move(root_deps)
    };

    // Topological sort via DFS post-order
    std::unordered_map<std::string, Mark> marks;
    std::vector<std::string> order;
    std::vector<std::string> cycle_stack;

    if (!topo_visit(root_canonical, graph, marks, order, cycle_stack, result.diagnostics, filename)) {
        result.success = false;
        return result;
    }

    // Also visit any modules not reachable from root (shouldn't happen, but be safe)
    for (const auto& [path, _] : graph) {
        if (marks[path] != Mark::Done) {
            if (!topo_visit(path, graph, marks, order, cycle_stack, result.diagnostics, filename)) {
                result.success = false;
                return result;
            }
        }
    }

    // Build output: topo order, excluding the root module
    for (const auto& path : order) {
        if (path == root_canonical) continue;
        auto it = graph.find(path);
        if (it != graph.end()) {
            result.modules.push_back(ResolvedModule{
                .canonical_path = it->second.canonical_path,
                .source = std::move(it->second.source)
            });
        }
    }

    // Blank root source
    result.root_source = blank_imports(source, root_directives);

    return result;
}

} // namespace akkado

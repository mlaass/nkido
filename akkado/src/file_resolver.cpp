#include "akkado/file_resolver.hpp"

#include <algorithm>

#ifndef __EMSCRIPTEN__
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

namespace akkado {

// ============================================================================
// Helper: extract parent directory from a path string
// ============================================================================

static std::string parent_dir(std::string_view path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string_view::npos) return ".";
    if (pos == 0) return "/";
    return std::string(path.substr(0, pos));
}

static std::string join_path(std::string_view dir, std::string_view rel) {
    if (dir.empty() || dir == ".") return std::string(rel);
    std::string result(dir);
    if (result.back() != '/') result.push_back('/');
    result.append(rel);
    return result;
}

/// Simplistic path normalization (resolves . and ..)
static std::string normalize_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string segment;
    for (char c : path) {
        if (c == '/') {
            if (segment == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!segment.empty() && segment != ".") {
                parts.push_back(std::move(segment));
            }
            segment.clear();
        } else {
            segment.push_back(c);
        }
    }
    // Final segment
    if (segment == "..") {
        if (!parts.empty()) parts.pop_back();
    } else if (!segment.empty() && segment != ".") {
        parts.push_back(std::move(segment));
    }

    std::string result;
    if (!path.empty() && path[0] == '/') result.push_back('/');
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result.push_back('/');
        result.append(parts[i]);
    }
    return result.empty() ? "." : result;
}

// ============================================================================
// VirtualResolver
// ============================================================================

void VirtualResolver::register_module(std::string path, std::string source) {
    modules_[std::move(path)] = std::move(source);
}

void VirtualResolver::unregister_module(const std::string& path) {
    modules_.erase(path);
}

void VirtualResolver::clear() {
    modules_.clear();
}

std::optional<std::string> VirtualResolver::resolve(
    std::string_view import_path,
    std::string_view from_file) const {

    // Relative path: resolve relative to importing file's directory
    if (import_path.starts_with("./") || import_path.starts_with("../")) {
        std::string dir = parent_dir(from_file);
        std::string resolved = normalize_path(join_path(dir, import_path));

        if (modules_.contains(resolved)) return resolved;
        // Try with .ak extension
        std::string with_ext = resolved + ".ak";
        if (modules_.contains(with_ext)) return with_ext;
        return std::nullopt;
    }

    // Bare name: direct lookup
    std::string key(import_path);
    if (modules_.contains(key)) return key;
    // Try with .ak extension
    std::string with_ext = key + ".ak";
    if (modules_.contains(with_ext)) return with_ext;
    return std::nullopt;
}

std::optional<std::string> VirtualResolver::read(
    std::string_view canonical_path) const {

    auto it = modules_.find(std::string(canonical_path));
    if (it != modules_.end()) return it->second;
    return std::nullopt;
}

// ============================================================================
// FilesystemResolver
// ============================================================================

#ifndef __EMSCRIPTEN__

FilesystemResolver::FilesystemResolver(std::vector<std::string> search_paths)
    : search_paths_(std::move(search_paths)) {}

std::optional<std::string> FilesystemResolver::resolve(
    std::string_view import_path,
    std::string_view from_file) const {

    namespace fs = std::filesystem;

    auto try_path = [](const fs::path& p) -> std::optional<std::string> {
        std::error_code ec;
        if (fs::exists(p, ec)) {
            auto canonical = fs::canonical(p, ec);
            if (!ec) return canonical.string();
        }
        // Try with .ak extension
        fs::path with_ext = p;
        with_ext += ".ak";
        if (fs::exists(with_ext, ec)) {
            auto canonical = fs::canonical(with_ext, ec);
            if (!ec) return canonical.string();
        }
        return std::nullopt;
    };

    // Relative path: resolve relative to importing file
    if (import_path.starts_with("./") || import_path.starts_with("../")) {
        fs::path base = fs::path(from_file).parent_path();
        fs::path candidate = base / import_path;
        return try_path(candidate);
    }

    // Bare name: search paths in order
    for (const auto& dir : search_paths_) {
        fs::path candidate = fs::path(dir) / import_path;
        auto result = try_path(candidate);
        if (result) return result;
    }

    return std::nullopt;
}

std::optional<std::string> FilesystemResolver::read(
    std::string_view canonical_path) const {

    std::string path_str(canonical_path);
    std::ifstream file(path_str);
    if (!file) return std::nullopt;

    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

#endif // __EMSCRIPTEN__

} // namespace akkado

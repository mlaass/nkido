#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace akkado {

/// Abstract interface for resolving and reading imported modules.
class FileResolver {
public:
    virtual ~FileResolver() = default;

    /// Resolve an import path to a canonical path.
    /// @param import_path  The path from the import statement (e.g., "utils", "./helpers")
    /// @param from_file    The file containing the import (for relative resolution)
    /// @return Canonical path if found, nullopt otherwise
    virtual std::optional<std::string> resolve(
        std::string_view import_path,
        std::string_view from_file) const = 0;

    /// Read the contents of a resolved (canonical) module path.
    virtual std::optional<std::string> read(
        std::string_view canonical_path) const = 0;
};

/// In-memory resolver for testing and web/WASM use.
class VirtualResolver : public FileResolver {
public:
    void register_module(std::string path, std::string source);
    void unregister_module(const std::string& path);
    void clear();

    std::optional<std::string> resolve(
        std::string_view import_path,
        std::string_view from_file) const override;

    std::optional<std::string> read(
        std::string_view canonical_path) const override;

private:
    std::unordered_map<std::string, std::string> modules_;
};

#ifndef __EMSCRIPTEN__

/// Filesystem resolver for CLI use.
class FilesystemResolver : public FileResolver {
public:
    explicit FilesystemResolver(std::vector<std::string> search_paths);

    std::optional<std::string> resolve(
        std::string_view import_path,
        std::string_view from_file) const override;

    std::optional<std::string> read(
        std::string_view canonical_path) const override;

private:
    std::vector<std::string> search_paths_;
};

#endif // __EMSCRIPTEN__

} // namespace akkado

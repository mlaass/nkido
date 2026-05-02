#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
#include <span>
#include "diagnostics.hpp"
#include "codegen.hpp"  // For StateInitData

namespace akkado {

// Forward declarations
class SampleRegistry;
class FileResolver;

/// Akkado version information (injected via CMake compile definitions)
#ifndef AKKADO_VERSION_MAJOR
#define AKKADO_VERSION_MAJOR 0
#define AKKADO_VERSION_MINOR 0
#define AKKADO_VERSION_PATCH 0
#endif

#define AKKADO_STRINGIFY_(x) #x
#define AKKADO_STRINGIFY(x) AKKADO_STRINGIFY_(x)

struct Version {
    static constexpr int major = AKKADO_VERSION_MAJOR;
    static constexpr int minor = AKKADO_VERSION_MINOR;
    static constexpr int patch = AKKADO_VERSION_PATCH;

    static constexpr std::string_view string() {
        return AKKADO_STRINGIFY(AKKADO_VERSION_MAJOR) "."
               AKKADO_STRINGIFY(AKKADO_VERSION_MINOR) "."
               AKKADO_STRINGIFY(AKKADO_VERSION_PATCH);
    }
};

// RequiredSample is defined in codegen.hpp

/// Compilation result
struct CompileResult {
    bool success = false;
    std::vector<std::uint8_t> bytecode;
    std::vector<SourceLocation> source_locations;  // Parallel to bytecode instructions, tracks origin
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data for patterns
    std::vector<std::string> required_samples;  // Sample names used (for runtime loading) - legacy
    std::vector<RequiredSample> required_samples_extended;  // Sample refs with bank/variant info
    std::vector<RequiredSoundFont> required_soundfonts;  // SoundFont files needed at runtime
    // Source strings collected from in('...') calls in compile order (one entry per call,
    // empty string if the call had no argument). Hosts use this to switch input source.
    std::vector<std::string> required_input_sources;
    std::vector<ParamDecl> param_decls;  // Declared parameters for UI generation
    std::vector<VisualizationDecl> viz_decls;  // Declared visualizations for UI generation
    std::vector<BuiltinVarOverride> builtin_var_overrides;  // Builtin variable overrides (bpm, sr)
    // Wavetable banks declared via wt_load(). v1 keeps the *last* loaded bank
    // active; multi-bank routing is a v2 follow-up.
    std::vector<RequiredWavetable> required_wavetables;
    // URIs declared via top-level directives like samples("..."). Hosts iterate
    // these in source order, dispatch each by `kind` to the appropriate
    // registry, and block bytecode swap until every URI resolves.
    std::vector<UriRequest> required_uris;
};

/// Compile Akkado source code to Cedar bytecode
/// @param source The source code to compile
/// @param filename Optional filename for error reporting
/// @param sample_registry Optional sample registry for resolving sample names to IDs
/// @param resolver Optional file resolver for import statements
/// @return Compilation result with bytecode and diagnostics
CompileResult compile(std::string_view source, std::string_view filename = "<input>",
                     SampleRegistry* sample_registry = nullptr,
                     const FileResolver* resolver = nullptr);

/// Compile from file (creates a FilesystemResolver for the file's directory)
/// @param path Path to the source file
/// @param sample_registry Optional sample registry for resolving sample names to IDs
/// @param resolver Optional file resolver (if null, creates a FilesystemResolver)
/// @return Compilation result
CompileResult compile_file(const std::string& path,
                          SampleRegistry* sample_registry = nullptr,
                          const FileResolver* resolver = nullptr);

} // namespace akkado

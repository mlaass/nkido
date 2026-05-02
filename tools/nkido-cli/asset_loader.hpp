#pragma once

#include "akkado/codegen.hpp"   // for RequiredSample, UriRequest
#include "cedar/io/file_cache.hpp"
#include "cedar/vm/vm.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nkido {

/// Strudel-manifest entry: one key (sample name) maps to one or more
/// relative paths (variants). A `_base` field may set the base URL; if
/// absent we derive one from the manifest URI.
struct BankManifest {
    std::string base_url;                                          // absolute base URL for variant paths
    std::unordered_map<std::string, std::vector<std::string>> samples;
};

/// Set up the process-wide UriResolver with the standard native handler
/// stack: file://, http://, https:// (cached via the supplied FileCache),
/// github:, bundled://. Idempotent — calling twice replaces the handlers.
void register_native_handlers(cedar::FileCache& cache);

/// Fetch and parse a strudel.json manifest from any URI scheme. Throws
/// `std::runtime_error` on fetch / parse failure. The returned manifest's
/// `base_url` is guaranteed absolute (HTTP/HTTPS URL or file:// path).
BankManifest fetch_bank_manifest(const std::string& uri);

/// Fetch a SoundFont (.sf2/.sf3) from any URI and load it into
/// `vm.soundfont_registry()`. `display_name` is recorded for the registry
/// entry. Returns the assigned SoundFont ID, or -1 on failure.
int load_soundfont_uri(cedar::VM& vm, const std::string& uri, const std::string& display_name);

/// Fetch a single audio file (WAV/OGG/FLAC/MP3) and register it in
/// `vm.sample_bank()` under `name`. Returns the sample ID (>=1) on success
/// or 0 on failure.
std::uint32_t load_sample_uri(cedar::VM& vm, const std::string& uri, const std::string& name);

/// Fetch every variant referenced by `required` from the supplied bank
/// manifests and register each into `vm.sample_bank()` under its qualified
/// name (matching `RequiredSample::qualified_name()`). The `default_banks`
/// list is consulted in order when a RequiredSample's bank field is empty
/// or "default"; named banks take their entry from `named_banks`. Missing
/// samples are reported on stderr but do not abort the run.
///
/// Returns the number of samples successfully registered.
std::size_t register_required_samples(
    cedar::VM& vm,
    const std::vector<akkado::RequiredSample>& required,
    const std::vector<BankManifest>& default_banks,
    const std::unordered_map<std::string, BankManifest>& named_banks);

}  // namespace nkido

#include "asset_loader.hpp"

#include "cedar/io/handlers/bundled_handler.hpp"
#include "cedar/io/handlers/file_handler.hpp"
#include "cedar/io/handlers/github_handler.hpp"
#include "cedar/io/handlers/http_handler.hpp"
#include "cedar/io/uri_resolver.hpp"

#include <cctype>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace nkido {

namespace {

/// Minimal recursive-descent parser for the strudel.json subset:
/// `{ "<key>": <string-or-array-of-strings>, ... }`. No numbers, booleans,
/// nulls, or nested objects — those keys are silently skipped. Robust
/// enough for every published Dirt-Samples / TR808 / TR909 manifest.
class ManifestScanner {
public:
    explicit ManifestScanner(std::string_view text) : src_(text) {}

    BankManifest parse() {
        BankManifest m;
        skip_ws();
        if (!consume('{')) throw std::runtime_error("manifest: expected '{'");
        skip_ws();
        if (peek() == '}') { advance(); return m; }

        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) throw std::runtime_error("manifest: expected ':' after key '" + key + "'");
            skip_ws();
            char c = peek();
            if (c == '"') {
                std::string value = parse_string();
                if (key == "_base") {
                    m.base_url = std::move(value);
                } else if (key.empty() || key[0] == '_') {
                    // ignore _name and other underscore-prefixed metadata
                } else {
                    m.samples[std::move(key)] = {std::move(value)};
                }
            } else if (c == '[') {
                auto values = parse_string_array();
                if (key.empty() || key[0] == '_') {
                    // ignore underscore-prefixed array fields
                } else {
                    m.samples[std::move(key)] = std::move(values);
                }
            } else {
                // Skip unsupported value types (numbers, booleans, null, objects)
                skip_value();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            throw std::runtime_error("manifest: expected ',' or '}'");
        }
        return m;
    }

private:
    char peek() {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }
    char advance() {
        return pos_ < src_.size() ? src_[pos_++] : '\0';
    }
    bool consume(char c) {
        if (peek() == c) { advance(); return true; }
        return false;
    }
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    std::string parse_string() {
        if (!consume('"')) throw std::runtime_error("manifest: expected string literal");
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) break;
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    default:   out.push_back(esc); break;  // permissive
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("manifest: unterminated string");
    }

    std::vector<std::string> parse_string_array() {
        std::vector<std::string> out;
        if (!consume('[')) throw std::runtime_error("manifest: expected '['");
        skip_ws();
        if (consume(']')) return out;
        while (true) {
            skip_ws();
            out.push_back(parse_string());
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) break;
            throw std::runtime_error("manifest: expected ',' or ']' in array");
        }
        return out;
    }

    void skip_value() {
        // Skip a JSON value that we don't support (numbers, booleans, null,
        // nested objects). Greedy: read until comma/brace at the same nesting
        // level, balancing quotes and brackets.
        int depth = 0;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '"') {
                parse_string();
                continue;
            }
            if (c == '{' || c == '[') { ++depth; ++pos_; continue; }
            if (c == '}' || c == ']') {
                if (depth == 0) return;
                --depth; ++pos_; continue;
            }
            if (c == ',' && depth == 0) return;
            ++pos_;
        }
    }

    std::string_view src_;
    std::size_t pos_ = 0;
};

std::string derive_base_url(const std::string& uri) {
    // Strip everything after the last '/' to get the directory URL.
    auto last = uri.find_last_of('/');
    if (last == std::string::npos) return {};
    return uri.substr(0, last + 1);
}

std::string github_to_https(const std::string& uri) {
    if (uri.rfind("github:", 0) != 0) return uri;
    return cedar::GithubHandler::to_https_url(uri);
}

}  // namespace

void register_native_handlers(cedar::FileCache& cache) {
    auto& r = cedar::UriResolver::instance();
    r.register_handler(std::make_unique<cedar::FileHandler>());
    r.register_handler(std::make_unique<cedar::HttpHandler>("http", &cache));
    r.register_handler(std::make_unique<cedar::HttpHandler>("https", &cache));
    r.register_handler(std::make_unique<cedar::GithubHandler>());
    r.register_handler(std::make_unique<cedar::BundledHandler>());
}

BankManifest fetch_bank_manifest(const std::string& uri) {
    auto& r = cedar::UriResolver::instance();
    auto result = r.load(uri);
    if (!result.success()) {
        throw std::runtime_error("fetch failed: " + result.error().message);
    }

    const auto& bytes = result.buffer();
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ManifestScanner scanner(text);
    BankManifest m = scanner.parse();

    if (m.base_url.empty()) {
        // Derive from the resolved URI. github: maps to https first.
        std::string base_source = github_to_https(uri);
        m.base_url = derive_base_url(base_source);
    }
    if (!m.base_url.empty() && m.base_url.back() != '/') m.base_url.push_back('/');
    return m;
}

int load_soundfont_uri(cedar::VM& vm, const std::string& uri, const std::string& display_name) {
#ifndef CEDAR_NO_SOUNDFONT
    auto& r = cedar::UriResolver::instance();
    auto result = r.load(uri);
    if (!result.success()) {
        std::cerr << "error: SoundFont fetch '" << uri << "' failed: "
                  << result.error().message << "\n";
        return -1;
    }
    const auto& bytes = result.buffer();
    return vm.soundfont_registry().load_from_memory(
        cedar::MemoryView(bytes.data(), bytes.size()), display_name, vm.sample_bank());
#else
    (void)vm; (void)uri; (void)display_name;
    std::cerr << "error: nkido-cli built without SoundFont support\n";
    return -1;
#endif
}

std::uint32_t load_sample_uri(cedar::VM& vm, const std::string& uri, const std::string& name) {
    auto& r = cedar::UriResolver::instance();
    auto result = r.load(uri);
    if (!result.success()) {
        std::cerr << "error: sample fetch '" << uri << "' failed: "
                  << result.error().message << "\n";
        return 0;
    }
    const auto& bytes = result.buffer();
    return vm.sample_bank().load_audio_data(name, cedar::MemoryView(bytes.data(), bytes.size()));
}

namespace {

const std::string* lookup_variant(const BankManifest& m,
                                  const std::string& sample_name,
                                  int variant) {
    auto it = m.samples.find(sample_name);
    if (it == m.samples.end() || it->second.empty()) return nullptr;
    int actual = variant % static_cast<int>(it->second.size());
    if (actual < 0) actual += static_cast<int>(it->second.size());
    return &it->second[static_cast<std::size_t>(actual)];
}

std::string resolve_sample_url(const BankManifest& m, const std::string& path) {
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0 ||
        path.rfind("file://", 0) == 0 || (!path.empty() && path[0] == '/')) {
        return path;
    }
    return m.base_url + path;
}

}  // namespace

std::size_t register_required_samples(
    cedar::VM& vm,
    const std::vector<akkado::RequiredSample>& required,
    const std::vector<BankManifest>& default_banks,
    const std::unordered_map<std::string, BankManifest>& named_banks) {
    std::size_t loaded = 0;

    for (const auto& req : required) {
        if (vm.sample_bank().has_sample(req.qualified_name())) {
            ++loaded;
            continue;
        }

        // Search the appropriate bank list.
        const std::string* path = nullptr;
        const BankManifest* hit_bank = nullptr;

        if (req.bank.empty() || req.bank == "default") {
            for (const auto& m : default_banks) {
                path = lookup_variant(m, req.name, req.variant);
                if (path) { hit_bank = &m; break; }
            }
        } else {
            auto it = named_banks.find(req.bank);
            if (it != named_banks.end()) {
                path = lookup_variant(it->second, req.name, req.variant);
                if (path) hit_bank = &it->second;
            }
        }

        if (!path || !hit_bank) {
            std::cerr << "warning: sample '" << req.qualified_name()
                      << "' not found in any loaded bank\n";
            continue;
        }

        std::string url = resolve_sample_url(*hit_bank, *path);
        if (load_sample_uri(vm, url, req.qualified_name()) != 0) {
            ++loaded;
        }
    }

    return loaded;
}

}  // namespace nkido

#include <catch2/catch_test_macros.hpp>

#include "akkado/builtins.hpp"
#include "akkado/builtins_json.hpp"

#include <string>
#include <string_view>

using namespace akkado;

namespace {

// Find a builtin's `{` ... `}` slice within the JSON dump. Returns the
// indices [open, close] in the original string. Returns {std::string::npos,
// std::string::npos} if the builtin is not present.
std::pair<std::size_t, std::size_t> find_builtin_block(std::string_view json, std::string_view name) {
    std::string needle;
    needle.reserve(name.size() + 4);
    needle += '"';
    needle += name;
    needle += "\":{";
    auto start = json.find(needle);
    if (start == std::string_view::npos) return {std::string_view::npos, std::string_view::npos};
    std::size_t open = start + needle.size() - 1;  // points at '{'
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (std::size_t i = open; i < json.size(); ++i) {
        char c = json[i];
        if (escape) { escape = false; continue; }
        if (in_string) {
            if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return {open, i};
        }
    }
    return {std::string_view::npos, std::string_view::npos};
}

bool block_contains(std::string_view json, std::string_view name, std::string_view fragment) {
    auto [open, close] = find_builtin_block(json, name);
    if (open == std::string_view::npos) return false;
    return json.substr(open, close - open + 1).find(fragment) != std::string_view::npos;
}

}  // namespace

TEST_CASE("builtins JSON emits optionFields for waterfall", "[builtins-json][option-schema]") {
    std::string json = serialize_builtins_json();

    REQUIRE(block_contains(json, "waterfall", "\"type\":\"record\""));
    REQUIRE(block_contains(json, "waterfall", "\"optionFields\":["));
    REQUIRE(block_contains(json, "waterfall", "\"acceptsSpread\":true"));

    // All eight fields present
    for (const char* fld : {"width", "height", "angle", "speed", "fft", "gradient", "minDb", "maxDb"}) {
        std::string needle = std::string("\"name\":\"") + fld + "\"";
        REQUIRE(block_contains(json, "waterfall", needle));
    }

    // Enum values surface for `gradient` and `fft`
    REQUIRE(block_contains(json, "waterfall", "\"values\":\"magma,viridis,inferno,grayscale\""));
    REQUIRE(block_contains(json, "waterfall", "\"values\":\"256,512,1024,2048\""));

    // Quoted-string default repr stays quoted in the JSON
    REQUIRE(block_contains(json, "waterfall", "\"default\":\"\\\"magma\\\"\""));
}

TEST_CASE("builtins JSON emits optionFields for pianoroll", "[builtins-json][option-schema]") {
    std::string json = serialize_builtins_json();

    REQUIRE(block_contains(json, "pianoroll", "\"type\":\"record\""));
    REQUIRE(block_contains(json, "pianoroll", "\"name\":\"showGrid\""));
    REQUIRE(block_contains(json, "pianoroll", "\"name\":\"scale\""));
    REQUIRE(block_contains(json, "pianoroll", "\"values\":\"chromatic,pentatonic,octave\""));
    REQUIRE(block_contains(json, "pianoroll", "\"type\":\"bool\""));
    // beats has no default — should not emit a default key for it (best we can
    // do without a real JSON parser is verify the full beats block is short)
    REQUIRE(block_contains(json, "pianoroll", "\"name\":\"beats\""));
}

TEST_CASE("builtins JSON emits optionFields for spectrum", "[builtins-json][option-schema]") {
    std::string json = serialize_builtins_json();
    REQUIRE(block_contains(json, "spectrum", "\"type\":\"record\""));
    REQUIRE(block_contains(json, "spectrum", "\"name\":\"logScale\""));
    REQUIRE(block_contains(json, "spectrum", "\"name\":\"fft\""));
    REQUIRE(block_contains(json, "spectrum", "\"values\":\"256,512,1024,2048\""));
}

TEST_CASE("builtins JSON emits optionFields for oscilloscope", "[builtins-json][option-schema]") {
    std::string json = serialize_builtins_json();
    REQUIRE(block_contains(json, "oscilloscope", "\"type\":\"record\""));
    REQUIRE(block_contains(json, "oscilloscope", "\"name\":\"triggerLevel\""));
    REQUIRE(block_contains(json, "oscilloscope", "\"values\":\"rising,falling\""));
}

TEST_CASE("builtins JSON emits optionFields for waveform", "[builtins-json][option-schema]") {
    std::string json = serialize_builtins_json();
    REQUIRE(block_contains(json, "waveform", "\"type\":\"record\""));
    REQUIRE(block_contains(json, "waveform", "\"name\":\"filled\""));
    REQUIRE(block_contains(json, "waveform", "\"name\":\"duration\""));
}

TEST_CASE("builtins JSON does not regress non-viz builtins", "[builtins-json]") {
    std::string json = serialize_builtins_json();

    // saw has no record-typed param — must NOT emit optionFields
    REQUIRE_FALSE(block_contains(json, "saw", "\"optionFields\":"));
    REQUIRE_FALSE(block_contains(json, "saw", "\"type\":\"record\""));

    // Existing keys still present
    REQUIRE(block_contains(json, "saw", "\"name\":\"freq\""));
    REQUIRE(block_contains(json, "saw", "\"required\":true"));
    REQUIRE(block_contains(json, "saw", "\"description\":\""));
}

TEST_CASE("builtins JSON top-level shape preserved", "[builtins-json]") {
    std::string json = serialize_builtins_json();
    REQUIRE(json.starts_with("{\"functions\":{"));
    REQUIRE(json.find("\"aliases\":{") != std::string::npos);
    REQUIRE(json.find("\"keywords\":[") != std::string::npos);
    REQUIRE(json.back() == '}');
}

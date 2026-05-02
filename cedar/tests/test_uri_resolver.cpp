#include <catch2/catch_test_macros.hpp>

#include <cedar/io/uri_resolver.hpp>
#include <cedar/io/handlers/bundled_handler.hpp>

#include <cedar/io/handlers/github_handler.hpp>

#ifndef __EMSCRIPTEN__
#include <cedar/io/handlers/file_handler.hpp>
#include <cedar/io/handlers/http_handler.hpp>
#include <cedar/io/file_cache.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#endif

#include <memory>
#include <string>
#include <vector>

using namespace cedar;

// ============================================================================
// Scheme parsing [uri-resolver]
// ============================================================================

TEST_CASE("UriResolver::extract_scheme parses URIs", "[uri-resolver]") {
    CHECK(UriResolver::extract_scheme("file:///abs/path") == "file");
    CHECK(UriResolver::extract_scheme("https://example.com/x") == "https");
    CHECK(UriResolver::extract_scheme("github:user/repo") == "github");
    CHECK(UriResolver::extract_scheme("bundled://default-808") == "bundled");
    CHECK(UriResolver::extract_scheme("bundled:default-808") == "bundled");

    SECTION("bare paths return empty scheme") {
        CHECK(UriResolver::extract_scheme("/abs/path/file.wav").empty());
        CHECK(UriResolver::extract_scheme("./rel/file.wav").empty());
        CHECK(UriResolver::extract_scheme("../rel/file.wav").empty());
    }

    SECTION("Windows drive letters are not parsed as schemes") {
        CHECK(UriResolver::extract_scheme("C:\\Windows\\file.wav").empty());
        CHECK(UriResolver::extract_scheme("C:/Windows/file.wav").empty());
        CHECK(UriResolver::extract_scheme("d:/dir").empty());
    }

    SECTION("empty input returns empty scheme") {
        CHECK(UriResolver::extract_scheme("").empty());
    }
}

// ============================================================================
// Resolver dispatch [uri-resolver]
// ============================================================================

namespace {

/// Stub handler that records the URIs it was called with.
class RecordingHandler final : public UriHandler {
public:
    explicit RecordingHandler(std::string scheme, std::string tag = "")
        : scheme_(std::move(scheme)), tag_(std::move(tag)) {}

    [[nodiscard]] std::string_view scheme() const override { return scheme_; }
    [[nodiscard]] LoadResult load(std::string_view uri) const override {
        last_uri_ = std::string(uri);
        std::string body = tag_ + ":" + last_uri_;
        std::vector<std::uint8_t> bytes(body.begin(), body.end());
        return {OwnedBuffer(std::move(bytes))};
    }

    mutable std::string last_uri_;

private:
    std::string scheme_;
    std::string tag_;
};

/// Make a fresh resolver-like object by clearing the singleton's table.
/// The singleton is process-global; tests touch it sequentially so we
/// register handlers per-test and rely on "last registration wins".
UriResolver& fresh_resolver() {
    return UriResolver::instance();
}

}  // namespace

TEST_CASE("UriResolver dispatches by scheme", "[uri-resolver]") {
    auto& r = fresh_resolver();
    r.register_handler(std::make_unique<RecordingHandler>("test1", "A"));
    r.register_handler(std::make_unique<RecordingHandler>("test2", "B"));

    auto a = r.load("test1:foo");
    REQUIRE(a.success());
    CHECK(std::string(a.buffer().view().begin(), a.buffer().view().end()) == "A:test1:foo");

    auto b = r.load("test2://bar");
    REQUIRE(b.success());
    CHECK(std::string(b.buffer().view().begin(), b.buffer().view().end()) == "B:test2://bar");
}

TEST_CASE("UriResolver returns UnsupportedFormat for unknown scheme", "[uri-resolver]") {
    auto& r = fresh_resolver();
    auto result = r.load("definitely-not-a-real-scheme-xyz://foo");
    REQUIRE_FALSE(result.success());
    CHECK(result.error().code == FileError::UnsupportedFormat);
}

TEST_CASE("UriResolver returns InvalidFormat for empty URI", "[uri-resolver]") {
    auto& r = fresh_resolver();
    auto result = r.load("");
    REQUIRE_FALSE(result.success());
    CHECK(result.error().code == FileError::InvalidFormat);
}

TEST_CASE("UriResolver: last registration wins", "[uri-resolver]") {
    auto& r = fresh_resolver();
    r.register_handler(std::make_unique<RecordingHandler>("dup-scheme", "FIRST"));
    r.register_handler(std::make_unique<RecordingHandler>("dup-scheme", "SECOND"));

    auto result = r.load("dup-scheme:x");
    REQUIRE(result.success());
    auto view = result.buffer().view();
    CHECK(std::string(view.begin(), view.end()) == "SECOND:dup-scheme:x");
}

TEST_CASE("UriResolver: scheme matching is case-insensitive", "[uri-resolver]") {
    auto& r = fresh_resolver();
    r.register_handler(std::make_unique<RecordingHandler>("MixedCase", "X"));

    auto a = r.load("mixedcase:foo");
    auto b = r.load("MIXEDCASE://bar");
    auto c = r.load("MixedCase:baz");
    CHECK(a.success());
    CHECK(b.success());
    CHECK(c.success());
}

// ============================================================================
// BundledHandler [uri-resolver]
// ============================================================================

TEST_CASE("BundledHandler returns registered asset", "[uri-resolver][bundled]") {
    auto handler = std::make_unique<BundledHandler>();
    handler->register_asset("default-808",
                            std::vector<std::uint8_t>{1, 2, 3, 4});

    auto result = handler->load("bundled://default-808");
    REQUIRE(result.success());
    auto view = result.buffer().view();
    REQUIRE(view.size == 4);
    CHECK(view.data[0] == 1);
    CHECK(view.data[3] == 4);
}

TEST_CASE("BundledHandler accepts bundled: form too", "[uri-resolver][bundled]") {
    auto handler = std::make_unique<BundledHandler>();
    handler->register_asset("kit", std::vector<std::uint8_t>{42});

    auto a = handler->load("bundled:kit");
    auto b = handler->load("bundled://kit");
    REQUIRE(a.success());
    REQUIRE(b.success());
    CHECK(a.buffer().size() == 1);
    CHECK(b.buffer().size() == 1);
}

TEST_CASE("BundledHandler returns NotFound for missing name", "[uri-resolver][bundled]") {
    BundledHandler handler;
    auto result = handler.load("bundled://does-not-exist");
    REQUIRE_FALSE(result.success());
    CHECK(result.error().code == FileError::NotFound);
}

// ============================================================================
// FileHandler (native only) [uri-resolver]
// ============================================================================

#ifndef __EMSCRIPTEN__

TEST_CASE("FileHandler round-trips a file via the URI resolver", "[uri-resolver][file]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "cedar_uri_resolver_test";
    fs::create_directories(tmp);
    auto path = tmp / "hello.bin";
    {
        std::ofstream out(path, std::ios::binary);
        const char body[] = "URI";
        out.write(body, 3);
    }

    auto& r = fresh_resolver();
    r.register_handler(std::make_unique<FileHandler>());

    SECTION("file:// URL form") {
        auto result = r.load("file://" + path.string());
        REQUIRE(result.success());
        CHECK(result.buffer().size() == 3);
    }

    SECTION("bare path defaults to file://") {
        auto result = r.load(path.string());
        REQUIRE(result.success());
        CHECK(result.buffer().size() == 3);
    }

    SECTION("missing file returns NotFound") {
        auto missing = (tmp / "missing.bin").string();
        auto result = r.load("file://" + missing);
        REQUIRE_FALSE(result.success());
        CHECK(result.error().code == FileError::NotFound);
    }

    fs::remove_all(tmp);
}

// ============================================================================
// FileCache (native only) [uri-resolver]
// ============================================================================

TEST_CASE("FileCache round-trip", "[uri-resolver][cache]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "cedar_uri_cache_roundtrip";
    fs::remove_all(tmp);

    FileCache cache(tmp);

    SECTION("miss returns nullopt") {
        auto miss = cache.get("https://example.com/never-fetched");
        CHECK_FALSE(miss.has_value());
    }

    SECTION("set then get returns the same bytes") {
        std::vector<std::uint8_t> bytes{0xCA, 0xFE, 0xBA, 0xBE};
        cache.set("https://example.com/x", MemoryView(bytes.data(), bytes.size()));

        auto hit = cache.get("https://example.com/x");
        REQUIRE(hit.has_value());
        REQUIRE(hit->size() == 4);
        CHECK(hit->view().data[0] == 0xCA);
        CHECK(hit->view().data[3] == 0xBE);
    }

    SECTION("remove deletes an entry") {
        std::vector<std::uint8_t> bytes{1};
        cache.set("uri-a", MemoryView(bytes.data(), bytes.size()));
        REQUIRE(cache.get("uri-a").has_value());
        cache.remove("uri-a");
        CHECK_FALSE(cache.get("uri-a").has_value());
    }

    SECTION("clear empties the directory") {
        std::vector<std::uint8_t> bytes{1};
        cache.set("a", MemoryView(bytes.data(), bytes.size()));
        cache.set("b", MemoryView(bytes.data(), bytes.size()));
        cache.clear();
        CHECK(cache.total_size() == 0);
    }

    fs::remove_all(tmp);
}

// ============================================================================
// GithubHandler URL transform [uri-resolver]
// ============================================================================

TEST_CASE("GithubHandler::to_https_url transforms URIs", "[uri-resolver][github]") {
    using GH = GithubHandler;
    SECTION("user/repo defaults to main + strudel.json") {
        CHECK(GH::to_https_url("github:tidalcycles/Dirt-Samples")
              == "https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/main/strudel.json");
    }

    SECTION("user/repo/branch") {
        CHECK(GH::to_https_url("github:foo/bar/develop")
              == "https://raw.githubusercontent.com/foo/bar/develop/strudel.json");
    }

    SECTION("user/repo/branch/sub/dir") {
        CHECK(GH::to_https_url("github:foo/bar/main/sub/dir")
              == "https://raw.githubusercontent.com/foo/bar/main/sub/dir/strudel.json");
    }

    SECTION("audio extension fetched as-is") {
        CHECK(GH::to_https_url("github:foo/bar/main/path/file.wav")
              == "https://raw.githubusercontent.com/foo/bar/main/path/file.wav");
        CHECK(GH::to_https_url("github:foo/bar/main/x.sf2")
              == "https://raw.githubusercontent.com/foo/bar/main/x.sf2");
    }

    SECTION("malformed input returns empty") {
        CHECK(GH::to_https_url("github:").empty());
        CHECK(GH::to_https_url("github:onlyone").empty());
        CHECK(GH::to_https_url("notgithub:foo/bar").empty());
    }
}

// ============================================================================
// Live HTTP test (gated on env var) [uri-resolver][http][network]
// ============================================================================

TEST_CASE("HttpHandler fetches a real file", "[uri-resolver][http][network]") {
    if (!std::getenv("CEDAR_ENABLE_NETWORK_TESTS")) {
        SKIP("network tests disabled (set CEDAR_ENABLE_NETWORK_TESTS=1)");
    }

    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "cedar_http_test_cache";
    fs::remove_all(tmp);

    FileCache cache(tmp);
    auto& r = fresh_resolver();
    r.register_handler(std::make_unique<HttpHandler>("https", &cache));
    r.register_handler(std::make_unique<GithubHandler>());

    constexpr std::string_view kSmallBd =
        "https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/master/bd/BT0A0A7.wav";

    auto first = r.load(kSmallBd);
    if (!first.success()) {
        WARN("HTTP fetch failed: " << first.error().message);
    }
    REQUIRE(first.success());
    CHECK(first.buffer().size() > 0);

    auto second = r.load(kSmallBd);
    REQUIRE(second.success());
    CHECK(second.buffer().size() == first.buffer().size());

    // Both fetches stored the same single cache entry.
    CHECK(cache.total_size() == first.buffer().size());

    SECTION("github: scheme also resolves") {
        auto g = r.load("github:tidalcycles/Dirt-Samples/master/bd/BT0A0A7.wav");
        if (!g.success()) {
            WARN("github fetch failed: " << g.error().message);
        }
        REQUIRE(g.success());
        CHECK(g.buffer().size() == first.buffer().size());
    }

    fs::remove_all(tmp);
}

#endif  // __EMSCRIPTEN__

#include <catch2/catch_test_macros.hpp>
#include "akkado/source_map.hpp"

TEST_CASE("SourceMap region lookup", "[source_map]") {
    akkado::SourceMap map;
    // stdlib: 0..100, user: 100..200
    map.add_region("<stdlib>", 0, 100, 0);
    map.add_region("main.ak", 100, 100, 10);

    SECTION("find region for offset in stdlib") {
        auto* r = map.find_region(50);
        REQUIRE(r != nullptr);
        CHECK(r->filename == "<stdlib>");
    }

    SECTION("find region for offset in user code") {
        auto* r = map.find_region(150);
        REQUIRE(r != nullptr);
        CHECK(r->filename == "main.ak");
    }

    SECTION("find region at boundary") {
        auto* r = map.find_region(100);
        REQUIRE(r != nullptr);
        CHECK(r->filename == "main.ak");
    }

    SECTION("offset before all regions returns nullptr") {
        // Offset exactly at start should find stdlib
        auto* r = map.find_region(0);
        REQUIRE(r != nullptr);
        CHECK(r->filename == "<stdlib>");
    }

    SECTION("offset past all regions returns nullptr") {
        auto* r = map.find_region(300);
        CHECK(r == nullptr);
    }
}

TEST_CASE("SourceMap three regions", "[source_map]") {
    akkado::SourceMap map;
    // stdlib: 0..50, module: 50..120, user: 120..200
    map.add_region("<stdlib>", 0, 50, 0);
    map.add_region("utils.ak", 50, 70, 5);
    map.add_region("main.ak", 120, 80, 12);

    auto* r1 = map.find_region(25);
    REQUIRE(r1 != nullptr);
    CHECK(r1->filename == "<stdlib>");

    auto* r2 = map.find_region(80);
    REQUIRE(r2 != nullptr);
    CHECK(r2->filename == "utils.ak");

    auto* r3 = map.find_region(150);
    REQUIRE(r3 != nullptr);
    CHECK(r3->filename == "main.ak");
}

TEST_CASE("SourceMap adjust_all", "[source_map]") {
    akkado::SourceMap map;
    map.add_region("<stdlib>", 0, 100, 0);
    map.add_region("main.ak", 100, 100, 10);

    SECTION("user code diagnostic gets adjusted") {
        std::vector<akkado::Diagnostic> diags;
        diags.push_back(akkado::Diagnostic{
            .severity = akkado::Severity::Error,
            .code = "E001",
            .message = "test",
            .filename = "unknown",
            .location = {.line = 15, .column = 1, .offset = 150, .length = 5}
        });
        map.adjust_all(diags);

        CHECK(diags[0].filename == "main.ak");
        CHECK(diags[0].location.line == 5);     // 15 - 10
        CHECK(diags[0].location.offset == 50);  // 150 - 100
    }

    SECTION("stdlib diagnostic gets filename set") {
        std::vector<akkado::Diagnostic> diags;
        diags.push_back(akkado::Diagnostic{
            .severity = akkado::Severity::Error,
            .code = "E001",
            .message = "test",
            .filename = "unknown",
            .location = {.line = 3, .column = 1, .offset = 30, .length = 5}
        });
        map.adjust_all(diags);

        CHECK(diags[0].filename == "<stdlib>");
        CHECK(diags[0].location.line == 3);     // 3 - 0
        CHECK(diags[0].location.offset == 30);  // 30 - 0
    }

    SECTION("related diagnostics are adjusted") {
        std::vector<akkado::Diagnostic> diags;
        akkado::Diagnostic diag{
            .severity = akkado::Severity::Error,
            .code = "E001",
            .message = "test",
            .filename = "unknown",
            .location = {.line = 15, .column = 1, .offset = 150, .length = 5}
        };
        diag.related.push_back(akkado::Diagnostic::Related{
            .message = "related",
            .filename = "unknown",
            .location = {.line = 3, .column = 1, .offset = 30, .length = 5}
        });
        diags.push_back(std::move(diag));
        map.adjust_all(diags);

        CHECK(diags[0].filename == "main.ak");
        CHECK(diags[0].related[0].filename == "<stdlib>");
    }
}

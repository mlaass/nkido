#include <catch2/catch_test_macros.hpp>
#include "akkado/file_resolver.hpp"

TEST_CASE("VirtualResolver basic operations", "[file_resolver]") {
    akkado::VirtualResolver resolver;

    SECTION("resolve and read registered module") {
        resolver.register_module("utils", "fn double(x) -> x * 2");
        auto path = resolver.resolve("utils", "main.ak");
        REQUIRE(path.has_value());
        CHECK(*path == "utils");

        auto source = resolver.read(*path);
        REQUIRE(source.has_value());
        CHECK(*source == "fn double(x) -> x * 2");
    }

    SECTION("resolve missing module returns nullopt") {
        auto path = resolver.resolve("nonexistent", "main.ak");
        CHECK_FALSE(path.has_value());
    }

    SECTION("auto-append .ak extension") {
        resolver.register_module("utils.ak", "fn triple(x) -> x * 3");
        auto path = resolver.resolve("utils", "main.ak");
        REQUIRE(path.has_value());
        CHECK(*path == "utils.ak");
    }

    SECTION("exact match preferred over .ak extension") {
        resolver.register_module("utils", "exact match");
        resolver.register_module("utils.ak", "extension match");
        auto path = resolver.resolve("utils", "main.ak");
        REQUIRE(path.has_value());
        CHECK(*path == "utils");
    }

    SECTION("unregister module") {
        resolver.register_module("utils", "content");
        resolver.unregister_module("utils");
        auto path = resolver.resolve("utils", "main.ak");
        CHECK_FALSE(path.has_value());
    }

    SECTION("clear all modules") {
        resolver.register_module("a", "1");
        resolver.register_module("b", "2");
        resolver.clear();
        CHECK_FALSE(resolver.resolve("a", "main.ak").has_value());
        CHECK_FALSE(resolver.resolve("b", "main.ak").has_value());
    }
}

TEST_CASE("VirtualResolver relative paths", "[file_resolver]") {
    akkado::VirtualResolver resolver;

    SECTION("resolve ./relative from importing file") {
        resolver.register_module("lib/utils.ak", "fn helper() -> 42");
        auto path = resolver.resolve("./utils", "lib/main.ak");
        REQUIRE(path.has_value());
        CHECK(*path == "lib/utils.ak");
    }

    SECTION("resolve ../parent relative path") {
        resolver.register_module("utils.ak", "fn helper() -> 42");
        auto path = resolver.resolve("../utils", "lib/main.ak");
        REQUIRE(path.has_value());
        CHECK(*path == "utils.ak");
    }

    SECTION("relative path not found returns nullopt") {
        auto path = resolver.resolve("./missing", "lib/main.ak");
        CHECK_FALSE(path.has_value());
    }
}

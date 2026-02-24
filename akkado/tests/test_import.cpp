#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include "akkado/file_resolver.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>

// Helper to check if a diagnostic with a specific code exists
static bool has_diagnostic_code(const std::vector<akkado::Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& d : diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// Helper to decode float from PUSH_CONST instruction
static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

TEST_CASE("Import integration: compile with resolver", "[import]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("utils", "fn double(x) -> x * 2");

    SECTION("function from imported module is available") {
        auto result = akkado::compile(
            "import \"utils\"\ndouble(220)",
            "main.ak", nullptr, &resolver);
        REQUIRE(result.success);
        CHECK(result.bytecode.size() > 0);
    }

    SECTION("transitive imports work") {
        resolver.register_module("base", "fn identity(x) -> x");
        resolver.register_module("mid", "import \"base\"\nfn wrap(x) -> identity(x)");

        auto result = akkado::compile(
            "import \"mid\"\nwrap(440)",
            "main.ak", nullptr, &resolver);
        REQUIRE(result.success);
    }

    SECTION("circular import produces E500") {
        resolver.register_module("a", "import \"b\"\nfn fa() -> 1");
        resolver.register_module("b", "import \"a\"\nfn fb() -> 2");

        auto result = akkado::compile(
            "import \"a\"\nfa()",
            "main.ak", nullptr, &resolver);
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E500"));
    }

    SECTION("missing module produces E502") {
        auto result = akkado::compile(
            "import \"nonexistent\"\n42",
            "main.ak", nullptr, &resolver);
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E502"));
    }
}

TEST_CASE("Import integration: no resolver", "[import]") {
    SECTION("import without resolver produces E505") {
        auto result = akkado::compile(
            "import \"utils\"\n42",
            "main.ak", nullptr, nullptr);
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E505"));
    }

    SECTION("no imports without resolver works normally") {
        auto result = akkado::compile("42", "main.ak");
        REQUIRE(result.success);
        CHECK(result.bytecode.size() == sizeof(cedar::Instruction));
    }
}

TEST_CASE("Import integration: backward compatibility", "[import]") {
    SECTION("existing code compiles identically without resolver") {
        auto result = akkado::compile("saw(440)");
        REQUIRE(result.success);
        CHECK(result.bytecode.size() == 2 * sizeof(cedar::Instruction));
    }

    SECTION("existing code compiles identically with empty resolver") {
        akkado::VirtualResolver resolver;
        auto result = akkado::compile("saw(440)", "<input>", nullptr, &resolver);
        REQUIRE(result.success);
        CHECK(result.bytecode.size() == 2 * sizeof(cedar::Instruction));
    }
}

TEST_CASE("Import integration: error location mapping", "[import]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("utils", "fn double(x) -> x * 2");

    SECTION("error in user code has correct line number") {
        auto result = akkado::compile(
            "import \"utils\"\nundefined_fn(440)",
            "main.ak", nullptr, &resolver);
        // Should fail because undefined_fn doesn't exist
        // The error should point to line 2, not some offset line
        CHECK_FALSE(result.success);
        bool found_user_error = false;
        for (const auto& d : result.diagnostics) {
            if (d.filename == "main.ak" && d.location.line == 2) {
                found_user_error = true;
            }
        }
        CHECK(found_user_error);
    }
}

TEST_CASE("Import integration: parser E501 for late import", "[import]") {
    // When no resolver is used and import appears after code,
    // the parser should report E501
    SECTION("import after code produces E501") {
        auto result = akkado::compile("42\nimport \"utils\"");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E501"));
    }
}

TEST_CASE("Import integration: imported definitions accessible", "[import]") {
    akkado::VirtualResolver resolver;

    SECTION("imported constant is accessible") {
        resolver.register_module("constants", "pi = 3.14159");
        auto result = akkado::compile(
            "import \"constants\"\npi",
            "main.ak", nullptr, &resolver);
        REQUIRE(result.success);
    }

    SECTION("multiple imports all available") {
        resolver.register_module("mod_a", "fn fa() -> 100");
        resolver.register_module("mod_b", "fn fb() -> 200");
        auto result = akkado::compile(
            "import \"mod_a\"\nimport \"mod_b\"\nfa() + fb()",
            "main.ak", nullptr, &resolver);
        REQUIRE(result.success);
    }
}

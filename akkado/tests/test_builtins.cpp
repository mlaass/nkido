#include <catch2/catch_test_macros.hpp>
#include "akkado/builtins.hpp"

using namespace akkado;

TEST_CASE("BuiltinInfo methods", "[builtins]") {
    SECTION("total_params for osc") {
        const auto& osc = BUILTIN_FUNCTIONS.at("osc");
        // osc has 2 required + 3 optional = 5
        CHECK(osc.total_params() == 5);
    }

    SECTION("total_params for lp filter") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        // lp has 2 required + 1 optional = 3
        CHECK(lp.total_params() == 3);
    }

    SECTION("total_params for stateless functions") {
        const auto& add = BUILTIN_FUNCTIONS.at("add");
        // add has 2 required + 0 optional = 2
        CHECK(add.total_params() == 2);
    }

    SECTION("find_param by name - osc") {
        const auto& osc = BUILTIN_FUNCTIONS.at("osc");
        CHECK(osc.find_param("type") == 0);
        CHECK(osc.find_param("freq") == 1);
        CHECK(osc.find_param("pwm") == 2);
        CHECK(osc.find_param("phase") == 3);
        CHECK(osc.find_param("trig") == 4);
        CHECK(osc.find_param("nonexistent") == -1);
    }

    SECTION("find_param by name - lp") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK(lp.find_param("in") == 0);
        CHECK(lp.find_param("cut") == 1);
        CHECK(lp.find_param("q") == 2);
        CHECK(lp.find_param("nonexistent") == -1);
    }

    SECTION("find_param returns -1 for empty param names") {
        const auto& clock = BUILTIN_FUNCTIONS.at("clock");
        CHECK(clock.find_param("") == -1);
        CHECK(clock.find_param("anything") == -1);
    }

    SECTION("has_default for required params") {
        const auto& osc = BUILTIN_FUNCTIONS.at("osc");
        CHECK_FALSE(osc.has_default(0));  // type - required
        CHECK_FALSE(osc.has_default(1));  // freq - required
    }

    SECTION("has_default for optional params") {
        const auto& osc = BUILTIN_FUNCTIONS.at("osc");
        CHECK(osc.has_default(2));  // pwm - optional with default
        // phase and trig have NAN defaults
        CHECK_FALSE(osc.has_default(3));  // phase - NAN means no default
        CHECK_FALSE(osc.has_default(4));  // trig - NAN means no default
    }

    SECTION("has_default for lp filter") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK_FALSE(lp.has_default(0));  // in - required
        CHECK_FALSE(lp.has_default(1));  // cut - required
        CHECK(lp.has_default(2));        // q - optional with 0.707f default
    }

    SECTION("has_default index out of range") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        // Index >= MAX_BUILTIN_DEFAULTS + input_count
        CHECK_FALSE(lp.has_default(10));
    }

    SECTION("get_default returns correct values - osc") {
        const auto& osc = BUILTIN_FUNCTIONS.at("osc");
        CHECK(osc.get_default(2) == 0.5f);  // pwm default is 0.5
    }

    SECTION("get_default returns correct values - lp") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK(lp.get_default(2) == 0.707f);  // q default is 0.707
    }

    SECTION("get_default returns correct values - adsr") {
        const auto& adsr = BUILTIN_FUNCTIONS.at("adsr");
        // adsr: gate required, attack=0.01, decay=0.1, sustain=0.5 (defaults in slots 0,1,2)
        CHECK(adsr.get_default(1) == 0.01f);  // attack
        CHECK(adsr.get_default(2) == 0.1f);   // decay
        CHECK(adsr.get_default(3) == 0.5f);   // sustain
    }

    SECTION("requires_state flag") {
        const auto& osc = BUILTIN_FUNCTIONS.at("osc");
        CHECK(osc.requires_state == true);

        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK(lp.requires_state == true);

        const auto& add = BUILTIN_FUNCTIONS.at("add");
        CHECK(add.requires_state == false);

        const auto& sin_fn = BUILTIN_FUNCTIONS.at("sin");
        CHECK(sin_fn.requires_state == false);
    }
}

TEST_CASE("lookup_builtin", "[builtins]") {
    SECTION("finds direct name - lp") {
        const auto* lp = lookup_builtin("lp");
        REQUIRE(lp != nullptr);
        CHECK(lp->opcode == cedar::Opcode::FILTER_SVF_LP);
    }

    SECTION("finds direct name - osc") {
        const auto* osc = lookup_builtin("osc");
        REQUIRE(osc != nullptr);
        CHECK(osc->opcode == cedar::Opcode::OSC_SIN);  // placeholder opcode
    }

    SECTION("resolves alias - lowpass") {
        const auto* lowpass = lookup_builtin("lowpass");
        REQUIRE(lowpass != nullptr);
        CHECK(lowpass->opcode == cedar::Opcode::FILTER_SVF_LP);
    }

    SECTION("resolves alias - highpass") {
        const auto* highpass = lookup_builtin("highpass");
        REQUIRE(highpass != nullptr);
        CHECK(highpass->opcode == cedar::Opcode::FILTER_SVF_HP);
    }

    SECTION("resolves alias - triangle") {
        const auto* triangle = lookup_builtin("triangle");
        REQUIRE(triangle != nullptr);
        CHECK(triangle->opcode == cedar::Opcode::OSC_TRI);
    }

    SECTION("resolves alias - reverb to freeverb") {
        const auto* reverb = lookup_builtin("reverb");
        REQUIRE(reverb != nullptr);
        CHECK(reverb->opcode == cedar::Opcode::REVERB_FREEVERB);
    }

    SECTION("resolves alias - tb303 to diode") {
        const auto* tb303 = lookup_builtin("tb303");
        REQUIRE(tb303 != nullptr);
        CHECK(tb303->opcode == cedar::Opcode::FILTER_DIODE);
    }

    SECTION("returns nullptr for unknown") {
        CHECK(lookup_builtin("not_a_builtin") == nullptr);
    }

    SECTION("returns nullptr for empty string") {
        CHECK(lookup_builtin("") == nullptr);
    }
}

TEST_CASE("canonical_name", "[builtins]") {
    SECTION("resolves alias to canonical - filters") {
        CHECK(canonical_name("lowpass") == "lp");
        CHECK(canonical_name("highpass") == "hp");
        CHECK(canonical_name("bandpass") == "bp");
    }

    SECTION("resolves alias to canonical - oscillators") {
        CHECK(canonical_name("triangle") == "tri");
        CHECK(canonical_name("sawtooth") == "saw");
        CHECK(canonical_name("square") == "sqr");
    }

    SECTION("resolves alias to canonical - effects") {
        CHECK(canonical_name("reverb") == "freeverb");
        CHECK(canonical_name("plate") == "dattorro");
        CHECK(canonical_name("distort") == "saturate");
    }

    SECTION("resolves alias to canonical - dynamics") {
        CHECK(canonical_name("compress") == "comp");
        CHECK(canonical_name("compressor") == "comp");
        CHECK(canonical_name("limit") == "limiter");
    }

    SECTION("returns input for non-alias") {
        CHECK(canonical_name("lp") == "lp");
        CHECK(canonical_name("osc") == "osc");
        CHECK(canonical_name("unknown") == "unknown");
        CHECK(canonical_name("") == "");
    }
}

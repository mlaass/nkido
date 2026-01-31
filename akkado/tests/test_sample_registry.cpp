#include <catch2/catch_test_macros.hpp>
#include "akkado/sample_registry.hpp"

using namespace akkado;

TEST_CASE("SampleRegistry basic operations", "[sample_registry]") {
    SampleRegistry registry;

    SECTION("register and retrieve sample") {
        CHECK(registry.register_sample("kick", 1));
        CHECK(registry.get_id("kick") == 1);
        CHECK(registry.has_sample("kick"));
    }

    SECTION("get_id returns 0 for unknown sample") {
        CHECK(registry.get_id("nonexistent") == 0);
    }

    SECTION("duplicate registration fails") {
        registry.register_sample("snare", 2);
        CHECK_FALSE(registry.register_sample("snare", 3));
        // Original ID is preserved
        CHECK(registry.get_id("snare") == 2);
    }

    SECTION("has_sample returns false for unregistered") {
        CHECK_FALSE(registry.has_sample("unknown"));
    }

    SECTION("get_all_names returns registered names") {
        registry.register_sample("bd", 1);
        registry.register_sample("sd", 2);
        auto names = registry.get_all_names();
        CHECK(names.size() == 2);
        // Check both names are present (order not guaranteed)
        bool has_bd = false, has_sd = false;
        for (const auto& name : names) {
            if (name == "bd") has_bd = true;
            if (name == "sd") has_sd = true;
        }
        CHECK(has_bd);
        CHECK(has_sd);
    }

    SECTION("clear removes all samples") {
        registry.register_sample("test", 1);
        registry.clear();
        CHECK(registry.size() == 0);
        CHECK_FALSE(registry.has_sample("test"));
    }

    SECTION("size tracks count") {
        CHECK(registry.size() == 0);
        registry.register_sample("a", 1);
        CHECK(registry.size() == 1);
        registry.register_sample("b", 2);
        CHECK(registry.size() == 2);
    }

    SECTION("register_defaults populates common samples") {
        registry.register_defaults();
        // Check drum samples
        CHECK(registry.has_sample("bd"));
        CHECK(registry.has_sample("kick"));  // alias for bd
        CHECK(registry.get_id("bd") == registry.get_id("kick"));

        CHECK(registry.has_sample("sd"));
        CHECK(registry.has_sample("snare"));  // alias for sd
        CHECK(registry.get_id("sd") == registry.get_id("snare"));

        CHECK(registry.has_sample("hh"));
        CHECK(registry.has_sample("hihat"));  // alias for hh

        CHECK(registry.has_sample("oh"));
        CHECK(registry.has_sample("cp"));
        CHECK(registry.has_sample("clap"));  // alias for cp
        CHECK(registry.has_sample("rim"));
        CHECK(registry.has_sample("tom"));
        CHECK(registry.has_sample("perc"));
        CHECK(registry.has_sample("cymbal"));
        CHECK(registry.has_sample("crash"));

        // Additional percussion
        CHECK(registry.has_sample("cowbell"));
        CHECK(registry.has_sample("shaker"));
        CHECK(registry.has_sample("tambourine"));
        CHECK(registry.has_sample("conga"));
        CHECK(registry.has_sample("bongo"));

        // Should have 19 entries (15 unique + 4 aliases)
        CHECK(registry.size() == 19);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/pattern_event.hpp"

using namespace akkado;
using Catch::Matchers::WithinRel;

TEST_CASE("PatternEvent type checks", "[pattern_event]") {
    SECTION("is_rest") {
        PatternEvent rest{.type = PatternEventType::Rest};
        PatternEvent pitch{.type = PatternEventType::Pitch};
        CHECK(rest.is_rest());
        CHECK_FALSE(pitch.is_rest());
    }

    SECTION("is_pitch") {
        PatternEvent pitch{.type = PatternEventType::Pitch};
        PatternEvent rest{.type = PatternEventType::Rest};
        CHECK(pitch.is_pitch());
        CHECK_FALSE(rest.is_pitch());
    }

    SECTION("is_sample") {
        PatternEvent sample{.type = PatternEventType::Sample};
        PatternEvent rest{.type = PatternEventType::Rest};
        CHECK(sample.is_sample());
        CHECK_FALSE(rest.is_sample());
    }

    SECTION("is_chord") {
        PatternEvent chord{.type = PatternEventType::Chord};
        PatternEvent rest{.type = PatternEventType::Rest};
        CHECK(chord.is_chord());
        CHECK_FALSE(rest.is_chord());
    }

    SECTION("should_trigger with full chance") {
        PatternEvent event{.chance = 1.0f};
        CHECK(event.should_trigger(0.0f));
        CHECK(event.should_trigger(0.5f));
        CHECK(event.should_trigger(0.99f));
        CHECK_FALSE(event.should_trigger(1.0f));  // Edge case: random_value >= chance
    }

    SECTION("should_trigger with partial chance") {
        PatternEvent event{.chance = 0.5f};
        CHECK(event.should_trigger(0.0f));
        CHECK(event.should_trigger(0.49f));
        CHECK_FALSE(event.should_trigger(0.5f));
        CHECK_FALSE(event.should_trigger(0.99f));
    }

    SECTION("should_trigger with zero chance") {
        PatternEvent event{.chance = 0.0f};
        CHECK_FALSE(event.should_trigger(0.0f));
    }
}

TEST_CASE("PatternEventStream operations", "[pattern_event]") {
    PatternEventStream stream;

    SECTION("empty stream") {
        CHECK(stream.empty());
        CHECK(stream.size() == 0);
    }

    SECTION("add events") {
        stream.add(PatternEvent{.type = PatternEventType::Pitch});
        CHECK_FALSE(stream.empty());
        CHECK(stream.size() == 1);
    }

    SECTION("add multiple events") {
        stream.add(PatternEvent{.type = PatternEventType::Pitch});
        stream.add(PatternEvent{.type = PatternEventType::Sample});
        stream.add(PatternEvent{.type = PatternEventType::Rest});
        CHECK(stream.size() == 3);
    }

    SECTION("clear events") {
        stream.add(PatternEvent{});
        stream.add(PatternEvent{});
        stream.clear();
        CHECK(stream.empty());
        CHECK(stream.size() == 0);
    }
}

TEST_CASE("PatternEvalContext modifiers", "[pattern_event]") {
    PatternEvalContext ctx{
        .start_time = 0.0f,
        .duration = 1.0f,
        .velocity = 1.0f,
        .chance = 1.0f
    };

    SECTION("with_speed divides duration") {
        auto fast = ctx.with_speed(2.0f);
        CHECK(fast.duration == 0.5f);  // 1.0 / 2.0
        CHECK(fast.velocity == 1.0f);  // unchanged
        CHECK(fast.chance == 1.0f);    // unchanged
        CHECK(fast.start_time == 0.0f); // unchanged
    }

    SECTION("with_speed slow") {
        auto slow = ctx.with_speed(0.5f);
        CHECK(slow.duration == 2.0f);  // 1.0 / 0.5
    }

    SECTION("with_velocity multiplies velocity") {
        auto soft = ctx.with_velocity(0.5f);
        CHECK(soft.velocity == 0.5f);
        CHECK(soft.duration == 1.0f);  // unchanged
        CHECK(soft.chance == 1.0f);    // unchanged
    }

    SECTION("with_velocity stacks") {
        auto softer = ctx.with_velocity(0.8f).with_velocity(0.5f);
        CHECK_THAT(softer.velocity, WithinRel(0.4f, 0.001f));
    }

    SECTION("with_chance multiplies chance") {
        auto half = ctx.with_chance(0.5f);
        CHECK(half.chance == 0.5f);
        CHECK(half.duration == 1.0f);  // unchanged
        CHECK(half.velocity == 1.0f);  // unchanged
    }

    SECTION("with_chance stacks") {
        auto quarter = ctx.with_chance(0.5f).with_chance(0.5f);
        CHECK_THAT(quarter.chance, WithinRel(0.25f, 0.001f));
    }

    SECTION("subdivide creates child contexts") {
        auto first = ctx.subdivide(0, 4);
        CHECK_THAT(first.start_time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(first.duration, WithinRel(0.25f, 0.001f));

        auto second = ctx.subdivide(1, 4);
        CHECK_THAT(second.start_time, WithinRel(0.25f, 0.001f));
        CHECK_THAT(second.duration, WithinRel(0.25f, 0.001f));

        auto fourth = ctx.subdivide(3, 4);
        CHECK_THAT(fourth.start_time, WithinRel(0.75f, 0.001f));
        CHECK_THAT(fourth.duration, WithinRel(0.25f, 0.001f));
    }

    SECTION("subdivide preserves velocity and chance") {
        PatternEvalContext modified{
            .start_time = 0.0f,
            .duration = 1.0f,
            .velocity = 0.8f,
            .chance = 0.5f
        };
        auto child = modified.subdivide(0, 2);
        CHECK(child.velocity == 0.8f);
        CHECK(child.chance == 0.5f);
    }

    SECTION("inherit creates copy") {
        PatternEvalContext modified{
            .start_time = 0.25f,
            .duration = 0.5f,
            .velocity = 0.8f,
            .chance = 0.6f
        };
        auto copy = modified.inherit();
        CHECK(copy.start_time == 0.25f);
        CHECK(copy.duration == 0.5f);
        CHECK(copy.velocity == 0.8f);
        CHECK(copy.chance == 0.6f);
    }

    SECTION("subdivide_weighted with equal weights") {
        // 3 elements with weight 1 each = total 3
        auto first = ctx.subdivide_weighted(0.0f, 1.0f, 3.0f);
        CHECK_THAT(first.start_time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(first.duration, WithinRel(1.0f / 3.0f, 0.01f));

        auto second = ctx.subdivide_weighted(1.0f, 1.0f, 3.0f);
        CHECK_THAT(second.start_time, WithinRel(1.0f / 3.0f, 0.01f));
        CHECK_THAT(second.duration, WithinRel(1.0f / 3.0f, 0.01f));

        auto third = ctx.subdivide_weighted(2.0f, 1.0f, 3.0f);
        CHECK_THAT(third.start_time, WithinRel(2.0f / 3.0f, 0.01f));
        CHECK_THAT(third.duration, WithinRel(1.0f / 3.0f, 0.01f));
    }

    SECTION("subdivide_weighted with unequal weights") {
        // weights: 2, 1 = total 3
        // first element: accumulated=0, weight=2, total=3
        auto first = ctx.subdivide_weighted(0.0f, 2.0f, 3.0f);
        CHECK_THAT(first.start_time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(first.duration, WithinRel(2.0f / 3.0f, 0.01f));

        // second element: accumulated=2, weight=1, total=3
        auto second = ctx.subdivide_weighted(2.0f, 1.0f, 3.0f);
        CHECK_THAT(second.start_time, WithinRel(2.0f / 3.0f, 0.01f));
        CHECK_THAT(second.duration, WithinRel(1.0f / 3.0f, 0.01f));
    }

    SECTION("subdivide_weighted preserves velocity and chance") {
        PatternEvalContext modified{
            .start_time = 0.0f,
            .duration = 1.0f,
            .velocity = 0.7f,
            .chance = 0.9f
        };
        auto child = modified.subdivide_weighted(0.0f, 1.0f, 2.0f);
        CHECK(child.velocity == 0.7f);
        CHECK(child.chance == 0.9f);
    }
}

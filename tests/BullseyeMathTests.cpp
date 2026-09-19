#include "hook/BullseyeOverlay.h"

#include <doctest/doctest.h>

using namespace polish::bullseye_math;

TEST_CASE("EaseOutCubic: endpoints and clamping") {
    CHECK(EaseOutCubic(0.0f) == doctest::Approx(0.0f));
    CHECK(EaseOutCubic(1.0f) == doctest::Approx(1.0f));
    CHECK(EaseOutCubic(-1.0f) == doctest::Approx(0.0f));
    CHECK(EaseOutCubic(2.0f) == doctest::Approx(1.0f));
}

TEST_CASE("EaseOutCubic: monotonic increasing, and front-loaded") {
    float previous = EaseOutCubic(0.0f);
    for (float t = 0.05f; t <= 1.0f; t += 0.05f) {
        const float current = EaseOutCubic(t);
        CHECK(current >= previous);
        previous = current;
    }
    CHECK(EaseOutCubic(0.5f) > 0.5f);
}

TEST_CASE("RadiusAt: paste expands 0 -> max, copy collapses max -> 0") {
    CHECK(RadiusAt(0.0f, 100.0f, /*expanding=*/true) == doctest::Approx(0.0f));
    CHECK(RadiusAt(1.0f, 100.0f, /*expanding=*/true) == doctest::Approx(100.0f));
    CHECK(RadiusAt(0.0f, 100.0f, /*expanding=*/false) == doctest::Approx(100.0f));
    CHECK(RadiusAt(1.0f, 100.0f, /*expanding=*/false) == doctest::Approx(0.0f));
}

TEST_CASE("RadiusAt: copy is the exact reverse-radius of paste at every t") {
    for (float t = 0.0f; t <= 1.0f; t += 0.1f) {
        CHECK(RadiusAt(t, 100.0f, true) + RadiusAt(t, 100.0f, false) == doctest::Approx(100.0f));
    }
}

TEST_CASE("AlphaEnvelope: zero at both ends") {
    CHECK(AlphaEnvelope(0.0f, 200) == 0);
    CHECK(AlphaEnvelope(1.0f, 200) == 0);
    CHECK(AlphaEnvelope(-0.5f, 200) == 0);
    CHECK(AlphaEnvelope(1.5f, 200) == 0);
}

TEST_CASE("AlphaEnvelope: holds at peak through the middle") {
    CHECK(AlphaEnvelope(0.2f, 200) == 200);
    CHECK(AlphaEnvelope(0.4f, 200) == 200);
    CHECK(AlphaEnvelope(0.6f, 200) == 200);
}

TEST_CASE("AlphaEnvelope: ramps up monotonically, then fades down monotonically") {
    int previous = AlphaEnvelope(0.0f, 200);
    for (float t = 0.02f; t <= 0.2f; t += 0.02f) {
        const int current = AlphaEnvelope(t, 200);
        CHECK(current >= previous);
        previous = current;
    }
    previous = AlphaEnvelope(0.6f, 200);
    for (float t = 0.62f; t < 1.0f; t += 0.02f) {
        const int current = AlphaEnvelope(t, 200);
        CHECK(current <= previous);
        previous = current;
    }
}

TEST_CASE("RingDistance: zero on the circle, grows either side") {
    CHECK(RingDistance(10.0f, 0.0f, 10.0f) == doctest::Approx(0.0f));
    CHECK(RingDistance(0.0f, -10.0f, 10.0f) == doctest::Approx(0.0f));
    CHECK(RingDistance(13.0f, 0.0f, 10.0f) == doctest::Approx(3.0f));  // outside
    CHECK(RingDistance(7.0f, 0.0f, 10.0f) == doctest::Approx(3.0f));   // inside
    CHECK(RingDistance(0.0f, 0.0f, 10.0f) == doctest::Approx(10.0f));  // the centre
}

TEST_CASE("DistanceToAlpha: peak at d=0, zero at d=halfStroke") {
    CHECK(DistanceToAlpha(0.0f, 12.0f, 200) == 200);
    CHECK(DistanceToAlpha(12.0f, 12.0f, 200) == 0);
    CHECK(DistanceToAlpha(100.0f, 12.0f, 200) == 0);
}

TEST_CASE("DistanceToAlpha: monotonic decrease, degenerate stroke doesn't divide by zero") {
    int previous = DistanceToAlpha(0.0f, 12.0f, 200);
    for (float d = 1.0f; d <= 12.0f; d += 1.0f) {
        const int current = DistanceToAlpha(d, 12.0f, 200);
        CHECK(current <= previous);
        previous = current;
    }
    CHECK(DistanceToAlpha(0.0f, 0.0f, 200) == 200);
    CHECK(DistanceToAlpha(1.0f, 0.0f, 200) == 0);
}

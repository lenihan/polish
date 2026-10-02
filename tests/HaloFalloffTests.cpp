#include "hook/ActiveWindowHalo.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace polish;
using namespace polish::halo_math;

TEST_CASE("DistanceToAlpha: peak at d=0, zero at d=halo") {
    CHECK(DistanceToAlpha(0.0f, /*halo=*/24, /*peak=*/190) == 190);
    CHECK(DistanceToAlpha(24.0f, /*halo=*/24, /*peak=*/190) == 0);
}

TEST_CASE("DistanceToAlpha: monotonic decrease between d=0 and d=halo") {
    constexpr int halo = 24;
    constexpr int peak = 190;
    int previous = DistanceToAlpha(0.0f, halo, peak);
    for (float d = 1.0f; d <= static_cast<float>(halo); d += 1.0f) {
        const int current = DistanceToAlpha(d, halo, peak);
        CHECK(current <= previous);
        previous = current;
    }
}

TEST_CASE("DistanceToAlpha: beyond halo stays zero, degenerate halo doesn't divide by zero") {
    CHECK(DistanceToAlpha(100.0f, /*halo=*/24, /*peak=*/190) == 0);
    CHECK(DistanceToAlpha(-5.0f, /*halo=*/0, /*peak=*/190) == 190);
    CHECK(DistanceToAlpha(5.0f, /*halo=*/0, /*peak=*/190) == 0);
}

TEST_CASE("RoundedRectDistance: center of a large rect is deeply negative (interior)") {
    const float d = RoundedRectDistance(0.0f, 0.0f, /*halfWidth=*/100.0f, /*halfHeight=*/60.0f, /*radius=*/8.0f);
    CHECK(d < 0.0f);
}

TEST_CASE("RoundedRectDistance: interior returns negative even at radius=0") {
    const float d = RoundedRectDistance(0.0f, 0.0f, /*halfWidth=*/50.0f, /*halfHeight=*/50.0f, /*radius=*/0.0f);
    CHECK(d < 0.0f);
}

TEST_CASE("RoundedRectDistance: straight-edge point (away from any corner) matches 1D edge distance") {
    // Directly above the flat top edge's midpoint -- nowhere near a
    // corner, so distance should equal simple 1D distance to that edge.
    constexpr float halfWidth = 100.0f;
    constexpr float halfHeight = 60.0f;
    constexpr float radius = 8.0f;
    const float d = RoundedRectDistance(0.0f, -75.0f, halfWidth, halfHeight, radius);
    CHECK(d == doctest::Approx(75.0f - halfHeight));
}

TEST_CASE("RoundedRectDistance: corner distance matches the analytic arc distance") {
    // A point straight out along the corner's own 45-degree diagonal, at
    // arc-center distance `probe` from the rounded corner's arc center --
    // the true distance to the rounded shape there is exactly probe - r,
    // regardless of the rect's overall size.
    constexpr float halfWidth = 100.0f;
    constexpr float halfHeight = 60.0f;
    constexpr float radius = 8.0f;
    const float arcCenterX = halfWidth - radius;
    const float arcCenterY = halfHeight - radius;
    constexpr float probe = 20.0f;
    const float offset = probe / std::sqrt(2.0f);
    const float px = arcCenterX + offset;
    const float py = arcCenterY + offset;
    const float d = RoundedRectDistance(px, py, halfWidth, halfHeight, radius);
    CHECK(d == doctest::Approx(probe - radius).epsilon(0.001));
}

TEST_CASE("RoundedRectDistance: radius clamps to half-extent for a target smaller than 2x the radius") {
    // halfWidth/halfHeight = 3, requested radius 8 -- clamped internally to
    // min(halfWidth, halfHeight) = 3, so the shape becomes a full
    // (diamond-cornered-into-circle) capsule rather than producing a
    // nonsensical negative inner extent. At the exact center this must
    // still read as interior (negative), not blow up or read as exterior.
    const float d = RoundedRectDistance(0.0f, 0.0f, /*halfWidth=*/3.0f, /*halfHeight=*/3.0f, /*radius=*/8.0f);
    CHECK(d == doctest::Approx(-3.0f));
}

TEST_CASE("RoundedRectDistance: boundary point on the flat edge is exactly zero") {
    constexpr float halfWidth = 100.0f;
    constexpr float halfHeight = 60.0f;
    constexpr float radius = 8.0f;
    const float d = RoundedRectDistance(0.0f, halfHeight, halfWidth, halfHeight, radius);
    CHECK(d == doctest::Approx(0.0f));
}

TEST_CASE("OutlineAlpha: full across the ring, nothing beyond it") {
    CHECK(OutlineAlpha(0.0f, /*thickness=*/1, /*alpha=*/210) == 210);
    CHECK(OutlineAlpha(0.5f, /*thickness=*/1, /*alpha=*/210) == 105);
    CHECK(OutlineAlpha(1.0f, /*thickness=*/1, /*alpha=*/210) == 0);
    CHECK(OutlineAlpha(8.0f, /*thickness=*/1, /*alpha=*/210) == 0);
}

TEST_CASE("OutlineAlpha: nothing inside the target, where the underlap lives") {
    // Negative d is behind the target's own edge. Putting the opposite
    // tone there would reopen the uneven-edge bug kUnderlapDip closed.
    CHECK(OutlineAlpha(-0.1f, /*thickness=*/2, /*alpha=*/210) == 0);
    CHECK(OutlineAlpha(-4.0f, /*thickness=*/2, /*alpha=*/210) == 0);
}

TEST_CASE("OutlineAlpha: a thicker ring is only antialiased on its outer pixel") {
    constexpr int thickness = 3;
    constexpr int alpha = 210;
    CHECK(OutlineAlpha(0.0f, thickness, alpha) == alpha);
    CHECK(OutlineAlpha(1.0f, thickness, alpha) == alpha);
    CHECK(OutlineAlpha(2.0f, thickness, alpha) == alpha);  // last full pixel
    CHECK(OutlineAlpha(2.5f, thickness, alpha) == 105);    // half covered
    CHECK(OutlineAlpha(3.0f, thickness, alpha) == 0);
}

TEST_CASE("OutlineAlpha: a zero or negative thickness draws no ring at all") {
    CHECK(OutlineAlpha(0.0f, /*thickness=*/0, /*alpha=*/210) == 0);
    CHECK(OutlineAlpha(0.0f, /*thickness=*/-2, /*alpha=*/210) == 0);
}

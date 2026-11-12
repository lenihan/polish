#include "windowtracking/StickyEdges.h"

#include <doctest/doctest.h>

#include <vector>

using namespace polish;

namespace {

// An app window in the middle of a 1920x1080 monitor whose taskbar takes the
// bottom 40px, with a second monitor to its right.
constexpr RECT kApp{400, 300, 1200, 800};
constexpr RECT kWork{0, 0, 1920, 1040};
constexpr RECT kMonitor{0, 0, 1920, 1080};
constexpr int kThreshold = 100;

std::vector<Barrier> AppWorkMonitor() { return {{kApp}, {kWork}, {kMonitor}}; }

}  // namespace

TEST_CASE("StickyEdges: leaving the app is held at each edge") {
    const auto barriers = AppWorkMonitor();
    struct Case {
        POINT from, to, expect;
    };
    const Case cases[] = {
        {{1190, 500}, {1210, 500}, {1199, 500}},  // right
        {{410, 500}, {390, 500}, {400, 500}},     // left
        {{700, 310}, {700, 290}, {700, 300}},     // top
        {{700, 790}, {700, 810}, {700, 799}},     // bottom
    };
    for (const Case& c : cases) {
        StickyState state;
        const StickyResult r = StepSticky(barriers, c.from, c.to, kThreshold, state, 1000);
        CHECK(r.hold);
        CHECK(r.clamped.x == c.expect.x);
        CHECK(r.clamped.y == c.expect.y);
    }
}

TEST_CASE("StickyEdges: coming in is free, and so is moving inside") {
    const auto barriers = AppWorkMonitor();
    StickyState state;
    CHECK_FALSE(StepSticky(barriers, {1300, 500}, {1100, 500}, kThreshold, state, 0).hold);
    CHECK_FALSE(StepSticky(barriers, {500, 500}, {600, 600}, kThreshold, state, 0).hold);
}

TEST_CASE("StickyEdges: a diagonal move slides along the edge") {
    const auto barriers = AppWorkMonitor();
    StickyState state;
    const StickyResult r = StepSticky(barriers, {1195, 500}, {1215, 540}, kThreshold, state, 0);
    CHECK(r.hold);
    CHECK(r.clamped.x == 1199);
    CHECK(r.clamped.y == 540);
}

TEST_CASE("StickyEdges: pushing on breaks through at the threshold") {
    const auto barriers = AppWorkMonitor();
    StickyState state;
    ULONGLONG now = 0;
    // Held at the edge, each move pushes 30px past it.
    int holds = 0;
    for (; holds < 10; ++holds) {
        now += 10;
        const StickyResult r = StepSticky(barriers, {1199, 500}, {1229, 500}, kThreshold, state, now);
        if (!r.hold) {
            break;
        }
    }
    CHECK(holds == 3);  // 30, 60, 90 held; 120 >= 100 passes
}

TEST_CASE("StickyEdges: a pause or backing off starts the push over") {
    const auto barriers = AppWorkMonitor();
    StickyState state;
    CHECK(StepSticky(barriers, {1199, 500}, {1279, 500}, kThreshold, state, 0).hold);  // 80
    CHECK(StepSticky(barriers, {1199, 500}, {1279, 500}, kThreshold, state, 600).hold);  // reset: 80 again

    CHECK_FALSE(StepSticky(barriers, {1199, 500}, {1180, 500}, kThreshold, state, 650).hold);  // back off
    CHECK(state.pushed == 0);
    CHECK(StepSticky(barriers, {1199, 500}, {1279, 500}, kThreshold, state, 700).hold);  // 80, not 160
}

TEST_CASE("StickyEdges: once past the app, the taskbar edge holds next") {
    const auto barriers = AppWorkMonitor();
    StickyState state;
    // Below the app, still in the work area: moving into the taskbar is held.
    const StickyResult r = StepSticky(barriers, {700, 1030}, {700, 1060}, kThreshold, state, 0);
    CHECK(r.hold);
    CHECK(r.clamped.y == 1039);
    // From the taskbar, up is free.
    StickyState fresh;
    CHECK_FALSE(StepSticky(barriers, {700, 1060}, {700, 1000}, kThreshold, fresh, 0).hold);
}

TEST_CASE("StickyEdges: edges that coincide take one push, not two") {
    // An app snapped to the bottom: its edge is the work area's.
    const std::vector<Barrier> barriers{{RECT{0, 500, 960, 1040}}, {kWork}, {kMonitor}};
    StickyState state;
    // Each move would land 30px into the taskbar.
    CHECK(StepSticky(barriers, {500, 1039}, {500, 1069}, kThreshold, state, 0).hold);   // 30
    CHECK(StepSticky(barriers, {500, 1039}, {500, 1069}, kThreshold, state, 10).hold);  // 60
    CHECK(StepSticky(barriers, {500, 1039}, {500, 1069}, kThreshold, state, 20).hold);  // 90
    CHECK_FALSE(StepSticky(barriers, {500, 1039}, {500, 1069}, kThreshold, state, 30).hold);  // 120
}

TEST_CASE("StickyEdges: the seam between monitors holds, with different heights") {
    // Left monitor 1920x1080 (no taskbar here), right monitor 2560x1440 starting at x=1920.
    const std::vector<Barrier> barriers{{kMonitor}};
    StickyState state;
    const StickyResult r = StepSticky(barriers, {1915, 600}, {1940, 620}, kThreshold, state, 0);
    CHECK(r.hold);
    CHECK(r.clamped.x == 1919);
    CHECK(r.clamped.y == 620);
}

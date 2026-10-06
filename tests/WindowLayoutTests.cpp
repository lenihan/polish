#include "windowtracking/WindowLayout.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace polish;

namespace {

constexpr RECT kWork = {0, 0, 1920, 1080};
constexpr RECT kPortraitWork = {0, 0, 1080, 1920};

bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

long Area(const RECT& r) {
    return static_cast<long>(r.right - r.left) * static_cast<long>(r.bottom - r.top);
}

// The two properties every arrangement has to have, whatever the kind:
// the slots add up to the work area exactly, and none of them overlap.
// Together these catch a lost pixel, a seam and a double-booked slot,
// which is every way the integer division can go wrong.
void CheckTilesExactly(const std::vector<RECT>& rects, const RECT& work) {
    REQUIRE_FALSE(rects.empty());
    long total = 0;
    for (const RECT& r : rects) {
        CHECK(r.left >= work.left);
        CHECK(r.top >= work.top);
        CHECK(r.right <= work.right);
        CHECK(r.bottom <= work.bottom);
        CHECK(r.right > r.left);
        CHECK(r.bottom > r.top);
        total += Area(r);
    }
    CHECK(total == Area(work));

    for (size_t i = 0; i < rects.size(); ++i) {
        for (size_t j = i + 1; j < rects.size(); ++j) {
            const bool overlaps = rects[i].left < rects[j].right && rects[j].left < rects[i].right &&
                                  rects[i].top < rects[j].bottom && rects[j].top < rects[i].bottom;
            CHECK_FALSE(overlaps);
        }
    }
}

}  // namespace

TEST_CASE("SlotsFor: each kind takes exactly as many windows as its name says") {
    CHECK(SlotsFor(ArrangeKind::TwoWay) == 2);
    CHECK(SlotsFor(ArrangeKind::ThreeWay) == 3);
    CHECK(SlotsFor(ArrangeKind::FourWay) == 4);
}

TEST_CASE("OrientationOf: a square work area counts as landscape") {
    CHECK(OrientationOf(kWork) == Orientation::Landscape);
    CHECK(OrientationOf(kPortraitWork) == Orientation::Portrait);
    // Pinned so the tie-break cannot change without a test noticing.
    CHECK(OrientationOf(RECT{0, 0, 1000, 1000}) == Orientation::Landscape);
    // One pixel either side of square.
    CHECK(OrientationOf(RECT{0, 0, 1001, 1000}) == Orientation::Landscape);
    CHECK(OrientationOf(RECT{0, 0, 1000, 1001}) == Orientation::Portrait);
}

TEST_CASE("ArrangeRects: two windows split the long axis") {
    // Halves and quarters are what gets asked for by name, so the exact
    // rects are spelled out rather than only checked for the tiling
    // properties.
    const auto halves = ArrangeRects(kWork, ArrangeKind::TwoWay, 2);
    REQUIRE(halves.size() == 2);
    CHECK(Same(halves[0], RECT{0, 0, 960, 1080}));
    CHECK(Same(halves[1], RECT{960, 0, 1920, 1080}));
    CheckTilesExactly(halves, kWork);
}

TEST_CASE("ArrangeRects: on a portrait monitor two windows split top and bottom instead") {
    const auto halves = ArrangeRects(kPortraitWork, ArrangeKind::TwoWay, 2);
    REQUIRE(halves.size() == 2);
    CHECK(Same(halves[0], RECT{0, 0, 1080, 960}));
    CHECK(Same(halves[1], RECT{0, 960, 1080, 1920}));
    CheckTilesExactly(halves, kPortraitWork);
}

TEST_CASE("ArrangeRects: three windows are three equal full-height columns") {
    const auto thirds = ArrangeRects(kWork, ArrangeKind::ThreeWay, 3);
    REQUIRE(thirds.size() == 3);
    CHECK(Same(thirds[0], RECT{0, 0, 640, 1080}));
    CHECK(Same(thirds[1], RECT{640, 0, 1280, 1080}));
    CHECK(Same(thirds[2], RECT{1280, 0, 1920, 1080}));
    CheckTilesExactly(thirds, kWork);
}

TEST_CASE("ArrangeRects: three windows on a portrait monitor are three equal rows") {
    const auto thirds = ArrangeRects(kPortraitWork, ArrangeKind::ThreeWay, 3);
    REQUIRE(thirds.size() == 3);
    CHECK(Same(thirds[0], RECT{0, 0, 1080, 640}));
    CHECK(Same(thirds[1], RECT{0, 640, 1080, 1280}));
    CHECK(Same(thirds[2], RECT{0, 1280, 1080, 1920}));
    CheckTilesExactly(thirds, kPortraitWork);
}

TEST_CASE("ArrangeRects: three columns meet exactly when the width does not divide by three") {
    // 1001 / 3 is where a naive width/3 loses a pixel and leaves a seam
    // the user can see as a strip of desktop between two windows.
    constexpr RECT awkward = {0, 0, 1001, 900};
    const auto thirds = ArrangeRects(awkward, ArrangeKind::ThreeWay, 3);
    REQUIRE(thirds.size() == 3);
    CheckTilesExactly(thirds, awkward);
    CHECK(thirds[0].right == thirds[1].left);
    CHECK(thirds[1].right == thirds[2].left);
    CHECK(thirds[2].right == awkward.right);
    // The widths differ by at most one pixel, so "equal thirds" is honest.
    const long widths[] = {thirds[0].right - thirds[0].left, thirds[1].right - thirds[1].left,
                           thirds[2].right - thirds[2].left};
    CHECK(*std::max_element(widths, widths + 3) - *std::min_element(widths, widths + 3) <= 1);
}

TEST_CASE("ArrangeRects: four windows are the four corners") {
    const auto quarters = ArrangeRects(kWork, ArrangeKind::FourWay, 4);
    REQUIRE(quarters.size() == 4);
    CHECK(Same(quarters[0], RECT{0, 0, 960, 540}));
    CHECK(Same(quarters[1], RECT{960, 0, 1920, 540}));
    CHECK(Same(quarters[2], RECT{0, 540, 960, 1080}));
    CHECK(Same(quarters[3], RECT{960, 540, 1920, 1080}));
    CheckTilesExactly(quarters, kWork);
}

TEST_CASE("ArrangeRects: four windows are the corners on a portrait monitor too") {
    // Orientation deliberately does NOT affect this case -- a 2x2 is a 2x2
    // whichever way round the monitor is.
    const auto quarters = ArrangeRects(kPortraitWork, ArrangeKind::FourWay, 4);
    REQUIRE(quarters.size() == 4);
    CHECK(Same(quarters[0], RECT{0, 0, 540, 960}));
    CHECK(Same(quarters[1], RECT{540, 0, 1080, 960}));
    CHECK(Same(quarters[2], RECT{0, 960, 540, 1920}));
    CHECK(Same(quarters[3], RECT{540, 960, 1080, 1920}));
    CheckTilesExactly(quarters, kPortraitWork);
}

TEST_CASE("ArrangeRects: an ultrawide monitor still gets four quarters, not four columns") {
    // The case that rules out nesting SliceAlongLongAxis -- each half of
    // this work area is still landscape. See ArrangeRects' comment.
    constexpr RECT ultrawide = {0, 0, 3840, 1080};
    const auto quarters = ArrangeRects(ultrawide, ArrangeKind::FourWay, 4);
    REQUIRE(quarters.size() == 4);
    CHECK(quarters[0].bottom == 540);
    CHECK(quarters[2].top == 540);
    CheckTilesExactly(quarters, ultrawide);
}

TEST_CASE("ArrangeRects: a work area with awkward dimensions still tiles exactly") {
    constexpr RECT odd = {7, 13, 1201, 907};
    CheckTilesExactly(ArrangeRects(odd, ArrangeKind::TwoWay, 2), odd);
    CheckTilesExactly(ArrangeRects(odd, ArrangeKind::ThreeWay, 3), odd);
    CheckTilesExactly(ArrangeRects(odd, ArrangeKind::FourWay, 4), odd);
}

TEST_CASE("ArrangeRects: a work area offset from the origin keeps its offset") {
    // Any monitor that is not the primary one has a non-zero origin, and
    // one to the left of the primary has a negative one.
    constexpr RECT secondary = {-1920, -200, 0, 880};
    const auto halves = ArrangeRects(secondary, ArrangeKind::TwoWay, 2);
    REQUIRE(halves.size() == 2);
    CHECK(halves[0].left == -1920);
    CHECK(halves[1].right == 0);
    CheckTilesExactly(halves, secondary);
}

TEST_CASE("ArrangeRects: the wrong number of windows for the kind gives nothing back") {
    // The caller decides availability; this refuses rather than improvises,
    // so a window can never land in a slot nothing is tracking.
    for (int count : {0, 1, 3, 4, 5}) {
        CHECK(ArrangeRects(kWork, ArrangeKind::TwoWay, count).empty());
    }
    for (int count : {0, 1, 2, 4, 5}) {
        CHECK(ArrangeRects(kWork, ArrangeKind::ThreeWay, count).empty());
    }
    for (int count : {0, 1, 2, 3, 5}) {
        CHECK(ArrangeRects(kWork, ArrangeKind::FourWay, count).empty());
    }
}

TEST_CASE("ArrangeRects: an empty or inverted work area gives nothing back") {
    CHECK(ArrangeRects(RECT{0, 0, 0, 0}, ArrangeKind::TwoWay, 2).empty());
    CHECK(ArrangeRects(RECT{100, 100, 50, 50}, ArrangeKind::FourWay, 4).empty());
    CHECK(ArrangeRects(RECT{0, 0, 1920, 0}, ArrangeKind::ThreeWay, 3).empty());
}

TEST_CASE("SliceAlongLongAxis: degenerate counts and work areas give nothing back") {
    CHECK(SliceAlongLongAxis(kWork, 0).empty());
    CHECK(SliceAlongLongAxis(kWork, -1).empty());
    CHECK(SliceAlongLongAxis(RECT{0, 0, 0, 0}, 2).empty());
}

TEST_CASE("SliceAlongLongAxis: any number of slices tiles the work area exactly") {
    for (int n = 1; n <= 9; ++n) {
        CheckTilesExactly(SliceAlongLongAxis(kWork, n), kWork);
        CheckTilesExactly(SliceAlongLongAxis(kPortraitWork, n), kPortraitWork);
    }
}

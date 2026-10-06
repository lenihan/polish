#include "windowtracking/WindowLayout.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {

constexpr RECT kWork = {0, 0, 1920, 1080};

bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

long Area(const RECT& r) {
    return static_cast<long>(r.right - r.left) * static_cast<long>(r.bottom - r.top);
}

}  // namespace

TEST_CASE("TileShape: biased wide, so three windows are two over one") {
    CHECK(TileShape(1).cols == 1);
    CHECK(TileShape(1).rows == 1);
    CHECK(TileShape(2).cols == 2);
    CHECK(TileShape(2).rows == 1);
    CHECK(TileShape(3).cols == 2);
    CHECK(TileShape(3).rows == 2);
    CHECK(TileShape(4).cols == 2);
    CHECK(TileShape(4).rows == 2);
    CHECK(TileShape(5).cols == 3);
    CHECK(TileShape(5).rows == 2);
    CHECK(TileShape(9).cols == 3);
    CHECK(TileShape(9).rows == 3);
    CHECK(TileShape(0).cols == 0);
}

TEST_CASE("TileRects: two windows are a left/right split, four are the corners") {
    // These two cases are worth naming because they are what gets asked
    // for by name -- halves and four corners -- and they fall out of the
    // general grid rather than being special-cased.
    const auto halves = TileRects(kWork, 2);
    REQUIRE(halves.size() == 2);
    CHECK(Same(halves[0], RECT{0, 0, 960, 1080}));
    CHECK(Same(halves[1], RECT{960, 0, 1920, 1080}));

    const auto quarters = TileRects(kWork, 4);
    REQUIRE(quarters.size() == 4);
    CHECK(Same(quarters[0], RECT{0, 0, 960, 540}));
    CHECK(Same(quarters[1], RECT{960, 0, 1920, 540}));
    CHECK(Same(quarters[2], RECT{0, 540, 960, 1080}));
    CHECK(Same(quarters[3], RECT{960, 540, 1920, 1080}));
}

TEST_CASE("TileRects: a short final row is stretched rather than left with a hole") {
    const auto three = TileRects(kWork, 3);
    REQUIRE(three.size() == 3);
    CHECK(Same(three[0], RECT{0, 0, 960, 540}));
    CHECK(Same(three[1], RECT{960, 0, 1920, 540}));
    CHECK(Same(three[2], RECT{0, 540, 1920, 1080}));  // full width, no gap beside it
}

TEST_CASE("TileRects: the slots tile the work area exactly, whatever the count") {
    // No gaps and no overlaps: the areas must add up to the work area,
    // which is the property that rounding would quietly break.
    for (int count = 1; count <= 12; ++count) {
        const auto rects = TileRects(kWork, count);
        REQUIRE(static_cast<int>(rects.size()) == count);
        long total = 0;
        for (const RECT& r : rects) {
            CHECK(r.right > r.left);
            CHECK(r.bottom > r.top);
            CHECK(r.left >= kWork.left);
            CHECK(r.top >= kWork.top);
            CHECK(r.right <= kWork.right);
            CHECK(r.bottom <= kWork.bottom);
            total += Area(r);
        }
        CHECK(total == Area(kWork));
    }
}

TEST_CASE("TileRects: an odd work area still tiles exactly, with no seam") {
    constexpr RECT odd = {7, 11, 1921, 1081};
    const auto rects = TileRects(odd, 5);
    REQUIRE(rects.size() == 5);
    long total = 0;
    for (const RECT& r : rects) {
        total += Area(r);
    }
    CHECK(total == Area(odd));
    // Adjacent columns in the first row must share an edge exactly.
    CHECK(rects[0].right == rects[1].left);
    CHECK(rects[1].right == rects[2].left);
    CHECK(rects[2].right == odd.right);
}

TEST_CASE("TileRects: nothing to arrange gives nothing back") {
    CHECK(TileRects(kWork, 0).empty());
    CHECK(TileRects(RECT{0, 0, 0, 0}, 4).empty());
}

TEST_CASE("CascadeRects: same size, stepped down and right") {
    const auto rects = CascadeRects(kWork, 3, 30);
    REQUIRE(rects.size() == 3);
    const LONG w = rects[0].right - rects[0].left;
    const LONG h = rects[0].bottom - rects[0].top;
    for (const RECT& r : rects) {
        CHECK(r.right - r.left == w);
        CHECK(r.bottom - r.top == h);
    }
    CHECK(Same(rects[0], RECT{0, 0, 1152, 756}));
    CHECK(rects[1].left == 30);
    CHECK(rects[1].top == 30);
    CHECK(rects[2].left == 60);
    CHECK(rects[2].top == 60);
}

TEST_CASE("CascadeRects: the stack restarts rather than walking off the screen") {
    // The whole reason the wrap exists: without it a long run puts the
    // last windows somewhere nobody can reach them.
    const auto rects = CascadeRects(kWork, 40, 30);
    REQUIRE(rects.size() == 40);
    for (const RECT& r : rects) {
        CHECK(r.left >= kWork.left);
        CHECK(r.top >= kWork.top);
        CHECK(r.right <= kWork.right);
        CHECK(r.bottom <= kWork.bottom);
    }
    // Having wrapped, a later window shares a position with an earlier one.
    bool wrapped = false;
    for (size_t i = 1; i < rects.size(); ++i) {
        if (rects[i].left == rects[0].left && rects[i].top == rects[0].top) {
            wrapped = true;
            break;
        }
    }
    CHECK(wrapped);
}

TEST_CASE("CascadeRects: a step too big for the room still produces a stack") {
    const auto rects = CascadeRects(kWork, 4, 100000);
    REQUIRE(rects.size() == 4);
    for (const RECT& r : rects) {
        CHECK(Same(r, rects[0]));  // all in one place, but all on screen
        CHECK(r.right <= kWork.right);
        CHECK(r.bottom <= kWork.bottom);
    }
}

TEST_CASE("CascadeRects: nothing to arrange gives nothing back") {
    CHECK(CascadeRects(kWork, 0, 30).empty());
    CHECK(CascadeRects(RECT{0, 0, 0, 0}, 4, 30).empty());
}

#include "windowtracking/StackStripLayout.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {

constexpr RECT kStrip = {0, 0, 1000, 40};
constexpr RECT kColumn = {0, 0, 200, 600};

TabMetrics Metrics() {
    TabMetrics m;
    m.maxWidth = 200;
    m.floorWidth = 44;
    m.gap = 4;
    m.leadingPad = 6;
    m.crossPad = 4;
    m.rowHeight = 30;
    return m;
}

}  // namespace

TEST_CASE("TabRects: a few tabs get equal widths, capped at the maximum") {
    const auto rects = TabRects(kStrip, 3, StackAlignment::Horizontal, Metrics());
    REQUIRE(rects.size() == 3);
    // 1000 - 12 - 8 = 980 / 3 = 326 -> capped at 200.
    CHECK(rects[0].right - rects[0].left == 200);
    CHECK(rects[1].right - rects[1].left == 200);
    CHECK(rects[0].left == 6);                   // leadingPad
    CHECK(rects[1].left == rects[0].right + 4);  // gap
    CHECK(rects[0].top == 4);                    // crossPad
    CHECK(rects[0].bottom == 36);
}

TEST_CASE("TabRects: many tabs share the room equally") {
    const auto rects = TabRects(kStrip, 8, StackAlignment::Horizontal, Metrics());
    REQUIRE(rects.size() == 8);
    // (1000 - 12 - 28) / 8 = 120
    for (const RECT& r : rects) {
        CHECK(r.right - r.left == 120);
    }
}

TEST_CASE("TabRects: past the floor, tabs that do not fit are left off rather than drawn outside") {
    const auto rects = TabRects(kStrip, 60, StackAlignment::Horizontal, Metrics());
    REQUIRE_FALSE(rects.empty());
    CHECK(rects.size() < 60);
    for (const RECT& r : rects) {
        CHECK(r.left >= kStrip.left);
        CHECK(r.right <= kStrip.right);
        CHECK(r.right - r.left >= 44);
    }
}

TEST_CASE("TabRects: one tab, and no tabs") {
    CHECK(TabRects(kStrip, 1, StackAlignment::Horizontal, Metrics()).size() == 1);
    CHECK(TabRects(kStrip, 0, StackAlignment::Horizontal, Metrics()).empty());
    CHECK(TabRects(RECT{0, 0, 0, 0}, 3, StackAlignment::Horizontal, Metrics()).empty());
}

TEST_CASE("TabRects: vertical stacks fixed-height rows downward inside the column") {
    const auto rects = TabRects(kColumn, 3, StackAlignment::Vertical, Metrics());
    REQUIRE(rects.size() == 3);
    CHECK(rects[0].left == 4);
    CHECK(rects[0].right == 196);
    CHECK(rects[0].top == 6);
    CHECK(rects[0].bottom - rects[0].top == 30);
    CHECK(rects[1].top == rects[0].bottom + 4);
}

TEST_CASE("TabRects: vertical rows that do not fit are left off") {
    const auto rects = TabRects(kColumn, 100, StackAlignment::Vertical, Metrics());
    REQUIRE_FALSE(rects.empty());
    CHECK(rects.size() < 100);
    for (const RECT& r : rects) {
        CHECK(r.bottom <= kColumn.bottom);
    }
}

TEST_CASE("TabIndexAt: the centre of each tab") {
    const auto rects = TabRects(kStrip, 3, StackAlignment::Horizontal, Metrics());
    for (size_t i = 0; i < rects.size(); ++i) {
        const POINT centre{(rects[i].left + rects[i].right) / 2, (rects[i].top + rects[i].bottom) / 2};
        REQUIRE(TabIndexAt(rects, centre).has_value());
        CHECK(*TabIndexAt(rects, centre) == i);
    }
}

TEST_CASE("TabIndexAt: edges are half-open, and gaps and margins are nothing") {
    const auto rects = TabRects(kStrip, 2, StackAlignment::Horizontal, Metrics());
    REQUIRE(rects.size() == 2);
    const RECT& a = rects[0];
    CHECK(*TabIndexAt(rects, POINT{a.left, a.top}) == 0);                 // top-left corner is in
    CHECK_FALSE(TabIndexAt(rects, POINT{a.right, a.top}).has_value());    // right edge is out
    CHECK_FALSE(TabIndexAt(rects, POINT{a.left, a.bottom}).has_value());  // bottom edge is out
    CHECK(*TabIndexAt(rects, POINT{a.right - 1, a.bottom - 1}) == 0);
    CHECK_FALSE(TabIndexAt(rects, POINT{a.left - 1, a.top}).has_value());
    CHECK_FALSE(TabIndexAt(rects, POINT{a.right + 1, a.top + 5}).has_value());  // in the gap
    CHECK_FALSE(TabIndexAt(rects, POINT{a.left + 5, 0}).has_value());           // in the crossPad margin
    CHECK_FALSE(TabIndexAt(rects, POINT{5000, 5000}).has_value());
}

TEST_CASE("ReorderTargetIndex: follows the pointer across tabs") {
    const auto rects = TabRects(kStrip, 4, StackAlignment::Horizontal, Metrics());
    const auto centre = [&](size_t i) {
        return POINT{(rects[i].left + rects[i].right) / 2, (rects[i].top + rects[i].bottom) / 2};
    };
    // Dragging every tab to every other, in both directions.
    for (size_t from = 0; from < 4; ++from) {
        for (size_t to = 0; to < 4; ++to) {
            CHECK(ReorderTargetIndex(rects, from, centre(to), StackAlignment::Horizontal) == to);
        }
    }
}

TEST_CASE("ReorderTargetIndex: past either end goes to the first or last slot") {
    const auto rects = TabRects(kStrip, 3, StackAlignment::Horizontal, Metrics());
    CHECK(ReorderTargetIndex(rects, 2, POINT{0, 20}, StackAlignment::Horizontal) == 0);
    CHECK(ReorderTargetIndex(rects, 0, POINT{999, 20}, StackAlignment::Horizontal) == 2);
}

TEST_CASE("ReorderTargetIndex: over a gap it stays put, so a wobble does not shuffle tabs") {
    const auto rects = TabRects(kStrip, 3, StackAlignment::Horizontal, Metrics());
    const POINT inGap{rects[0].right + 2, 20};
    REQUIRE_FALSE(TabIndexAt(rects, inGap).has_value());
    CHECK(ReorderTargetIndex(rects, 1, inGap, StackAlignment::Horizontal) == 1);
    CHECK(ReorderTargetIndex(rects, 0, inGap, StackAlignment::Horizontal) == 0);
}

TEST_CASE("ReorderTargetIndex: vertical uses the vertical axis") {
    const auto rects = TabRects(kColumn, 3, StackAlignment::Vertical, Metrics());
    CHECK(ReorderTargetIndex(rects, 0, POINT{100, 1}, StackAlignment::Vertical) == 0);
    CHECK(ReorderTargetIndex(rects, 0, POINT{100, 599}, StackAlignment::Vertical) == 2);
}

TEST_CASE("DragTearsOut: inside the strip, and within the threshold of it, is a reorder") {
    CHECK_FALSE(DragTearsOut(kStrip, POINT{500, 20}, 30));
    CHECK_FALSE(DragTearsOut(kStrip, POINT{500, 40 + 29}, 30));  // below, within the threshold
    CHECK_FALSE(DragTearsOut(kStrip, POINT{500, -29}, 30));
}

TEST_CASE("DragTearsOut: well clear of the strip is a tear-out, on every side") {
    CHECK(DragTearsOut(kStrip, POINT{500, 40 + 30}, 30));
    CHECK(DragTearsOut(kStrip, POINT{500, -31}, 30));
    CHECK(DragTearsOut(kStrip, POINT{-31, 20}, 30));
    CHECK(DragTearsOut(kStrip, POINT{1000 + 30, 20}, 30));
}

TEST_CASE("DragTearsOut: the boundary is exactly one place") {
    // Reorder and tear-out are decided by this one function, so there is a
    // single boundary: 29 below the strip is a reorder, 30 is not.
    CHECK_FALSE(DragTearsOut(kStrip, POINT{500, 40 + 29}, 30));
    CHECK(DragTearsOut(kStrip, POINT{500, 40 + 30}, 30));
}

TEST_CASE("InsertionIndexAt: before or after a tab depending on which half the point is on") {
    const auto rects = TabRects(kStrip, 3, StackAlignment::Horizontal, Metrics());
    const auto mid = [&](size_t i) { return (rects[i].left + rects[i].right) / 2; };
    CHECK(InsertionIndexAt(rects, POINT{rects[0].left, 20}, StackAlignment::Horizontal) == 0);
    CHECK(InsertionIndexAt(rects, POINT{mid(0) + 1, 20}, StackAlignment::Horizontal) == 1);
    CHECK(InsertionIndexAt(rects, POINT{mid(1) - 1, 20}, StackAlignment::Horizontal) == 1);
    CHECK(InsertionIndexAt(rects, POINT{mid(2) + 1, 20}, StackAlignment::Horizontal) == 3);  // the end
    CHECK(InsertionIndexAt(rects, POINT{5000, 20}, StackAlignment::Horizontal) == 3);
}

TEST_CASE("InsertionIndexAt: no tabs yet means slot 0") {
    CHECK(InsertionIndexAt({}, POINT{10, 10}, StackAlignment::Horizontal) == 0);
}

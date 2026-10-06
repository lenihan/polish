#include "windowtracking/WindowLayout.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace polish;

namespace {

constexpr RECT kWork = {0, 0, 1920, 1080};
constexpr RECT kPortraitWork = {0, 0, 1080, 1920};
constexpr RECT kWork2880 = {0, 0, 2880, 1824};

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

TEST_CASE("DistributeWithMinimums: no minimums is an equal split that sums exactly") {
    CHECK(DistributeWithMinimums(2880, 3, {}) == std::vector<int>{960, 960, 960});
    const auto odd = DistributeWithMinimums(1001, 3, {});
    REQUIRE(odd.size() == 3);
    CHECK(odd[0] + odd[1] + odd[2] == 1001);
}

TEST_CASE("DistributeWithMinimums: a window that cannot shrink keeps its minimum and the others share the rest") {
    // The reported case: the middle window will not go below 1500 in a
    // 2880 work area, so the other two split the 1380 that is left.
    const auto sizes = DistributeWithMinimums(2880, 3, {0, 1500, 0});
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[1] == 1500);
    CHECK(sizes[0] == 690);
    CHECK(sizes[2] == 690);
}

TEST_CASE("DistributeWithMinimums: a minimum at or under the equal share changes nothing") {
    CHECK(DistributeWithMinimums(2880, 3, {960, 500, 100}) == std::vector<int>{960, 960, 960});
}

TEST_CASE("DistributeWithMinimums: fixing one window can push another over its limit") {
    // Equal shares are 1000 each. The first needs 1600, which leaves 700
    // each for the others -- and the second needs 800, so it is held too,
    // leaving 600 for the third.
    const auto sizes = DistributeWithMinimums(3000, 3, {1600, 800, 0});
    CHECK(sizes == std::vector<int>{1600, 800, 600});
}

TEST_CASE("DistributeWithMinimums: minimums that cannot all fit fall back to equal shares") {
    CHECK(DistributeWithMinimums(900, 3, {500, 500, 500}) == std::vector<int>{300, 300, 300});
}

TEST_CASE("DistributeWithMinimums: the sizes always sum to the total") {
    for (int total : {1001, 2880, 1919, 7}) {
        for (int a : {0, 300, 900}) {
            for (int b : {0, 500, 1200}) {
                const auto sizes = DistributeWithMinimums(total, 3, {a, 0, b});
                REQUIRE(sizes.size() == 3);
                CHECK(sizes[0] + sizes[1] + sizes[2] == total);
            }
        }
    }
}

TEST_CASE("ArrangeRectsWithMinimums: three-way holds the stubborn window at its minimum") {
    const std::vector<SIZE> mins = {{0, 0}, {1500, 0}, {0, 0}};
    const auto rects = ArrangeRectsWithMinimums(kWork2880, ArrangeKind::ThreeWay, 3, mins);
    REQUIRE(rects.size() == 3);
    CHECK(rects[1].right - rects[1].left == 1500);
    CHECK(rects[0].right - rects[0].left == 690);
    CHECK(rects[2].right - rects[2].left == 690);
    CheckTilesExactly(rects, kWork2880);
}

TEST_CASE("ArrangeRectsWithMinimums: the first window being the stubborn one works too") {
    const std::vector<SIZE> mins = {{1500, 0}, {0, 0}, {0, 0}};
    const auto rects = ArrangeRectsWithMinimums(kWork2880, ArrangeKind::ThreeWay, 3, mins);
    REQUIRE(rects.size() == 3);
    CHECK(rects[0].right - rects[0].left == 1500);
    CheckTilesExactly(rects, kWork2880);
}

TEST_CASE("ArrangeRectsWithMinimums: portrait uses the heights") {
    const std::vector<SIZE> mins = {{0, 0}, {0, 1500}, {0, 0}};
    const auto rects = ArrangeRectsWithMinimums(kPortraitWork, ArrangeKind::ThreeWay, 3, mins);
    REQUIRE(rects.size() == 3);
    CHECK(rects[1].bottom - rects[1].top == 1500);
    CheckTilesExactly(rects, kPortraitWork);
}

TEST_CASE("ArrangeRectsWithMinimums: two-way gives a stubborn window what it needs") {
    const std::vector<SIZE> mins = {{2000, 0}, {0, 0}};
    const auto rects = ArrangeRectsWithMinimums(kWork2880, ArrangeKind::TwoWay, 2, mins);
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].right - rects[0].left == 2000);
    CHECK(rects[1].right - rects[1].left == 880);
    CheckTilesExactly(rects, kWork2880);
}

TEST_CASE("ArrangeRectsWithMinimums: four-way grows the column and row a stubborn window sits in") {
    // Top-right needs 2000 wide and 1400 tall.
    const std::vector<SIZE> mins = {{0, 0}, {2000, 1400}, {0, 0}, {0, 0}};
    const auto rects = ArrangeRectsWithMinimums(kWork2880, ArrangeKind::FourWay, 4, mins);
    REQUIRE(rects.size() == 4);
    CHECK(rects[1].right - rects[1].left == 2000);
    CHECK(rects[1].bottom - rects[1].top == 1400);
    // The window below it shares its column, so it gets the same width.
    CHECK(rects[3].right - rects[3].left == 2000);
    CheckTilesExactly(rects, kWork2880);
}

TEST_CASE("ArrangeRectsWithMinimums: no minimums is identical to ArrangeRects") {
    for (ArrangeKind kind : {ArrangeKind::TwoWay, ArrangeKind::ThreeWay, ArrangeKind::FourWay}) {
        const int n = SlotsFor(kind);
        const std::vector<SIZE> zeros(static_cast<size_t>(n), SIZE{0, 0});
        const auto plain = ArrangeRects(kWork, kind, n);
        const auto withZeros = ArrangeRectsWithMinimums(kWork, kind, n, zeros);
        const auto withNone = ArrangeRectsWithMinimums(kWork, kind, n, {});
        REQUIRE(plain.size() == withZeros.size());
        REQUIRE(plain.size() == withNone.size());
        for (size_t i = 0; i < plain.size(); ++i) {
            CHECK(Same(plain[i], withZeros[i]));
            CHECK(Same(plain[i], withNone[i]));
        }
    }
}

TEST_CASE("CheckArrangeFit: no minimums, or minimums that fit, always fit") {
    CHECK(CheckArrangeFit(kWork2880, ArrangeKind::ThreeWay, 3, {}).fits);
    CHECK(CheckArrangeFit(kWork2880, ArrangeKind::ThreeWay, 3, {{900, 300}, {900, 300}, {900, 300}}).fits);
    // Exactly the whole width is still a fit.
    CHECK(CheckArrangeFit(kWork2880, ArrangeKind::ThreeWay, 3, {{960, 0}, {960, 0}, {960, 0}}).fits);
}

TEST_CASE("CheckArrangeFit: three windows that together need more than the width do not fit") {
    // The reported case's mirror image: 3 x 2000 in 2880.
    const auto fit = CheckArrangeFit(kWork2880, ArrangeKind::ThreeWay, 3, {{2000, 0}, {2000, 0}, {2000, 0}});
    CHECK_FALSE(fit.fits);
    CHECK(fit.tooWide);
    CHECK(fit.needed == 6000);
    CHECK(fit.available == 2880);
}

TEST_CASE("CheckArrangeFit: names the window with the largest minimum as the blocker") {
    const auto fit = CheckArrangeFit(kWork2880, ArrangeKind::ThreeWay, 3, {{1000, 0}, {2000, 0}, {1500, 0}});
    CHECK_FALSE(fit.fits);
    CHECK(fit.blocker == 1);
    CHECK(fit.blockerMin == 2000);
}

TEST_CASE("CheckArrangeFit: one stubborn window that still leaves room for the others fits") {
    CHECK(CheckArrangeFit(kWork2880, ArrangeKind::ThreeWay, 3, {{0, 0}, {1500, 0}, {0, 0}}).fits);
}

TEST_CASE("CheckArrangeFit: a window taller than the work area does not fit whatever the width") {
    const auto fit = CheckArrangeFit(kWork2880, ArrangeKind::TwoWay, 2, {{0, 0}, {0, 2400}});
    CHECK_FALSE(fit.fits);
    CHECK_FALSE(fit.tooWide);
    CHECK(fit.blocker == 1);
    CHECK(fit.needed == 2400);
    CHECK(fit.available == 1824);
}

TEST_CASE("CheckArrangeFit: portrait counts heights along the split") {
    const auto fit = CheckArrangeFit(kPortraitWork, ArrangeKind::ThreeWay, 3, {{0, 800}, {0, 800}, {0, 800}});
    CHECK_FALSE(fit.fits);
    CHECK_FALSE(fit.tooWide);
    CHECK(fit.needed == 2400);
}

TEST_CASE("CheckArrangeFit: four-way adds the widest minimum of each column") {
    // Top-left and bottom-left need 1600 and 1000: the left column is 1600.
    // Right column 1500. 3100 > 2880.
    const auto fit = CheckArrangeFit(kWork2880, ArrangeKind::FourWay, 4, {{1600, 0}, {1500, 0}, {1000, 0}, {0, 0}});
    CHECK_FALSE(fit.fits);
    CHECK(fit.needed == 3100);
    // Two wide windows in the SAME column need that width only once.
    CHECK(CheckArrangeFit(kWork2880, ArrangeKind::FourWay, 4, {{2000, 0}, {0, 0}, {2000, 0}, {0, 0}}).fits);
}

TEST_CASE("CheckArrangeFitAnyOrder: the reported Outlook case fits once Outlook shares a row with the tall window") {
    // Outlook needs 1286 tall; one other window needs 757; the rest are
    // small. In the order given Outlook is opposite the 757 window, so the
    // rows are 1286 + 757 = 2043 > 1824 and that order does not fit. But
    // Outlook beside the 757 window leaves a row of two small windows, and
    // 1286 + (small) fits. This was reported as impossible when only
    // rotations of the order were tried, because rotation can never bring
    // two opposite windows into the same row.
    const std::vector<SIZE> mins = {{600, 1286}, {600, 300}, {600, 757}, {600, 300}};
    CHECK_FALSE(CheckArrangeFit(kWork2880, ArrangeKind::FourWay, 4, mins).fits);
    CHECK(CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::FourWay, mins).fits);
}

TEST_CASE("CheckArrangeFitAnyOrder: four-way depends on which windows share a column") {
    // Two wide windows fit if they share a column (the width is needed
    // once), and the search finds that arrangement whatever order they
    // were given in.
    CHECK(CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::FourWay, {{2000, 0}, {2000, 0}, {0, 0}, {0, 0}}).fits);
    CHECK(CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::FourWay, {{2000, 0}, {0, 0}, {2000, 0}, {0, 0}}).fits);
}

TEST_CASE("CheckArrangeFitAnyOrder: a genuinely impossible set fails in every arrangement") {
    // Four wide windows: at most two share a column, so the other column
    // needs 2000 too and 4000 > 2880.
    CHECK_FALSE(CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::FourWay,
                                        {{2000, 0}, {2000, 0}, {2000, 0}, {2000, 0}})
                    .fits);
    // Three wide windows still cannot, since one of them is alone in its
    // own column next to a pair that already needs the width.
    CHECK_FALSE(CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::FourWay,
                                        {{2000, 0}, {2000, 0}, {2000, 0}, {0, 0}})
                    .fits);
}

TEST_CASE("CheckArrangeFitAnyOrder: two- and three-way behave exactly like CheckArrangeFit") {
    const std::vector<SIZE> mins = {{2000, 0}, {2000, 0}, {2000, 0}};
    CHECK(CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::ThreeWay, mins).fits ==
          CheckArrangeFit(kWork2880, ArrangeKind::ThreeWay, 3, mins).fits);
    CHECK(CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::ThreeWay, {{0, 0}, {1500, 0}, {0, 0}}).fits);
}

TEST_CASE("CheckArrangeFitAnyOrder: on failure the report is for the order as given") {
    const auto fit =
        CheckArrangeFitAnyOrder(kWork2880, ArrangeKind::FourWay, {{2000, 0}, {2000, 0}, {2000, 0}, {2000, 0}});
    CHECK_FALSE(fit.fits);
    CHECK(fit.blocker == 0);
}

TEST_CASE("CheckArrangeFit: a shortfall of a few pixels still fits") {
    // The reported case: Outlook needs 1286px tall and the VS Code windows
    // 542px, so two rows need 1828 on a 1824px work area. Four pixels
    // over; refusing it outright was worse than the 4px overlap.
    const std::vector<SIZE> mins = {{600, 542}, {600, 542}, {600, 1286}, {600, 1286}};
    CHECK(CheckArrangeFit(kWork2880, ArrangeKind::FourWay, 4, mins).fits);
}

TEST_CASE("CheckArrangeFit: a real shortfall is still refused") {
    // 1286 + 580 = 1866, which is 42px over 1824 -- past the 18px slack.
    const std::vector<SIZE> mins = {{600, 580}, {600, 580}, {600, 1286}, {600, 1286}};
    CHECK_FALSE(CheckArrangeFit(kWork2880, ArrangeKind::FourWay, 4, mins).fits);
}

TEST_CASE("CheckArrangeFit: the slack is one percent, and at least 12px") {
    // 2880 wide: slack is 28px. 2908 fits, 2909 does not.
    CHECK(CheckArrangeFit(kWork2880, ArrangeKind::TwoWay, 2, {{1454, 0}, {1454, 0}}).fits);
    CHECK_FALSE(CheckArrangeFit(kWork2880, ArrangeKind::TwoWay, 2, {{1455, 0}, {1454, 0}}).fits);
    // A small work area still gets the 12px floor: 1000 wide would give 10.
    constexpr RECT small = {0, 0, 1000, 600};
    CHECK(CheckArrangeFit(small, ArrangeKind::TwoWay, 2, {{506, 0}, {506, 0}}).fits);
    CHECK_FALSE(CheckArrangeFit(small, ArrangeKind::TwoWay, 2, {{507, 0}, {506, 0}}).fits);
}

TEST_CASE("DistributeWithMinimums: a small shortfall is shared in proportion") {
    // 1286 + 542 = 1828 into 1824: each gets about its share, none is
    // dropped to nothing, and the total is exact.
    const auto sizes = DistributeWithMinimums(1824, 2, {1286, 542});
    REQUIRE(sizes.size() == 2);
    CHECK(sizes[0] + sizes[1] == 1824);
    CHECK(sizes[0] >= 1286 - 5);
    CHECK(sizes[1] >= 542 - 5);
}

TEST_CASE("DistributeWithMinimums: scaling down does not squeeze a window with no minimum to nothing") {
    const auto sizes = DistributeWithMinimums(1000, 3, {0, 900, 900});
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] + sizes[1] + sizes[2] == 1000);
    // Not nothing: the floor gives it a real slot, though a small one when
    // the other two want 1800px between them out of 1000.
    CHECK(sizes[0] > 50);
}

TEST_CASE("ArrangeRectsWithMinimums: the reported four-way case tiles the screen exactly") {
    const std::vector<SIZE> mins = {{600, 542}, {600, 542}, {600, 1286}, {600, 1286}};
    const auto rects = ArrangeRectsWithMinimums(kWork2880, ArrangeKind::FourWay, 4, mins);
    REQUIRE(rects.size() == 4);
    CheckTilesExactly(rects, kWork2880);
}

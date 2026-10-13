#include "windowtracking/ScaleGeometry.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {

constexpr RECT kWork = {0, 0, 1920, 1040};
constexpr SIZE kReal = {1200, 800};

bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

TEST_CASE("ScaledSize: rounds, and is never smaller than 1x1") {
    const SIZE half = ScaledSize(kReal, 0.5);
    CHECK(half.cx == 600);
    CHECK(half.cy == 400);

    const SIZE third = ScaledSize({1000, 500}, 0.33);
    CHECK(third.cx == 330);
    CHECK(third.cy == 165);

    const SIZE tiny = ScaledSize({3, 3}, 0.1);
    CHECK(tiny.cx == 1);
    CHECK(tiny.cy == 1);
}

TEST_CASE("ScaledViewRect: anchored at the given top-left") {
    const RECT view = ScaledViewRect(kReal, 0.25, {100, 50});
    CHECK(Same(view, {100, 50, 400, 250}));
}

TEST_CASE("ScaleOf: inverts ScaledViewRect") {
    const RECT view = ScaledViewRect(kReal, 0.5, {0, 0});
    CHECK(ScaleOf(view, kReal) == doctest::Approx(0.5));
    CHECK(ScaleOf(view, {0, 0}) == doctest::Approx(1.0));  // degenerate real size
}

TEST_CASE("UnscaledRectAt: grows out of the thumbnail's top-left") {
    const RECT rect = UnscaledRectAt(kReal, {100, 100}, kWork);
    CHECK(Same(rect, {100, 100, 1300, 900}));
}

TEST_CASE("UnscaledRectAt: shifted left and up just enough to fit") {
    // View in the bottom-right corner: 1:1 would hang off both edges.
    const RECT rect = UnscaledRectAt(kReal, {1700, 900}, kWork);
    CHECK(rect.right == kWork.right);
    CHECK(rect.bottom == kWork.bottom);
    CHECK(rect.right - rect.left == kReal.cx);
    CHECK(rect.bottom - rect.top == kReal.cy);
}

TEST_CASE("UnscaledRectAt: a view that sits off the top-left is pulled in") {
    const RECT rect = UnscaledRectAt(kReal, {-300, -40}, kWork);
    CHECK(rect.left == 0);
    CHECK(rect.top == 0);
}

TEST_CASE("UnscaledRectAt: a window bigger than the work area is pinned, not shrunk") {
    const SIZE huge = {2500, 1500};
    const RECT rect = UnscaledRectAt(huge, {800, 400}, kWork);
    CHECK(rect.left == 0);
    CHECK(rect.top == 0);
    CHECK(rect.right - rect.left == huge.cx);
    CHECK(rect.bottom - rect.top == huge.cy);
}

TEST_CASE("UnscaledRectAt: respects a work area that does not start at the origin") {
    const RECT secondMonitor = {1920, 0, 3840, 1040};
    const RECT rect = UnscaledRectAt(kReal, {1800, 100}, secondMonitor);
    CHECK(rect.left == 1920);
    CHECK(rect.top == 100);
}

TEST_CASE("ClampViewToWorkArea: keeps size, moves just far enough") {
    const RECT view = {1800, 1000, 2000, 1100};
    const RECT clamped = ClampViewToWorkArea(view, kWork);
    CHECK(clamped.right == kWork.right);
    CHECK(clamped.bottom == kWork.bottom);
    CHECK(clamped.right - clamped.left == 200);
    CHECK(clamped.bottom - clamped.top == 100);

    const RECT inside = {10, 10, 110, 60};
    CHECK(Same(ClampViewToWorkArea(inside, kWork), inside));
}

TEST_CASE("CornerDragAspectLocked: bottom-right keeps the top-left fixed") {
    const RECT start = ScaledViewRect(kReal, 0.25, {100, 100});  // 300x200
    // Pointer pulled out to x=700 -> width 600 -> scale 0.5.
    const RECT dragged = CornerDragAspectLocked(start, Corner::BottomRight, {700, 330}, kReal);
    CHECK(dragged.left == 100);
    CHECK(dragged.top == 100);
    CHECK(dragged.right - dragged.left == 600);
    CHECK(dragged.bottom - dragged.top == 400);
}

TEST_CASE("CornerDragAspectLocked: top-left keeps the bottom-right fixed") {
    const RECT start = ScaledViewRect(kReal, 0.25, {500, 400});  // 500..800, 400..600
    // Anchor is (800, 600); pointer (200, 200) asks for 0.5 on both axes.
    const RECT dragged = CornerDragAspectLocked(start, Corner::TopLeft, {200, 200}, kReal);
    CHECK(dragged.right == 800);
    CHECK(dragged.bottom == 600);
    CHECK(dragged.right - dragged.left == 600);  // 0.5
    CHECK(dragged.bottom - dragged.top == 400);
}

TEST_CASE("CornerDragAspectLocked: top-right and bottom-left anchor the diagonal") {
    const RECT start = ScaledViewRect(kReal, 0.25, {500, 400});  // 500..800, 400..600
    const RECT topRight = CornerDragAspectLocked(start, Corner::TopRight, {1100, 200}, kReal);
    CHECK(topRight.left == 500);
    CHECK(topRight.bottom == 600);
    CHECK(topRight.right - topRight.left == 600);

    const RECT bottomLeft = CornerDragAspectLocked(start, Corner::BottomLeft, {200, 800}, kReal);
    CHECK(bottomLeft.right == 800);
    CHECK(bottomLeft.top == 400);
    CHECK(bottomLeft.right - bottomLeft.left == 600);
}

TEST_CASE("CornerDragAspectLocked: aspect ratio holds whatever the pointer does") {
    const RECT start = ScaledViewRect(kReal, 0.25, {100, 100});
    // A pointer far off-axis: the taller ask wins, the shape stays 3:2.
    const RECT dragged = CornerDragAspectLocked(start, Corner::BottomRight, {250, 900}, kReal);
    const double ratio = static_cast<double>(dragged.right - dragged.left) / (dragged.bottom - dragged.top);
    CHECK(ratio == doctest::Approx(1.5).epsilon(0.01));
}

TEST_CASE("CornerDragAspectLocked: clamped to the min and max scale") {
    const RECT start = ScaledViewRect(kReal, 0.25, {100, 100});

    // Pulled across the anchor: minimum, not inverted.
    const RECT crossed = CornerDragAspectLocked(start, Corner::BottomRight, {0, 0}, kReal);
    CHECK(crossed.right - crossed.left == 120);  // 0.10 of 1200
    CHECK(crossed.left == 100);

    // Dragged way past 1:1: capped at 1:1.
    const RECT huge = CornerDragAspectLocked(start, Corner::BottomRight, {5000, 5000}, kReal);
    CHECK(huge.right - huge.left == kReal.cx);
    CHECK(huge.bottom - huge.top == kReal.cy);
}

TEST_CASE("SnapScale: lands on a preset within tolerance, otherwise leaves it alone") {
    CHECK(SnapScale(0.51) == doctest::Approx(0.5));
    CHECK(SnapScale(0.32) == doctest::Approx(0.33));
    CHECK(SnapScale(0.26) == doctest::Approx(0.25));
    CHECK(SnapScale(0.40) == doctest::Approx(0.40));
    CHECK(SnapScale(0.98) == doctest::Approx(1.0));
    CHECK(SnapScale(0.72) == doctest::Approx(0.72));
}

TEST_CASE("StepScale: walks the presets down") {
    CHECK(*StepScale(1.0, -1) == doctest::Approx(0.5));
    CHECK(*StepScale(0.5, -1) == doctest::Approx(0.33));
    CHECK(*StepScale(0.33, -1) == doctest::Approx(0.25));
    // Already at the smallest: stays.
    CHECK(*StepScale(0.25, -1) == doctest::Approx(0.25));
    CHECK(*StepScale(0.15, -1) == doctest::Approx(0.15));
}

TEST_CASE("StepScale: walks the presets up, and past the largest means restore") {
    CHECK(*StepScale(0.25, +1) == doctest::Approx(0.33));
    CHECK(*StepScale(0.33, +1) == doctest::Approx(0.5));
    CHECK_FALSE(StepScale(0.5, +1).has_value());
    CHECK_FALSE(StepScale(1.0, +1).has_value());
}

TEST_CASE("StepScale: an arbitrary dragged scale steps to the nearest preset in that direction") {
    CHECK(*StepScale(0.40, -1) == doctest::Approx(0.33));
    CHECK(*StepScale(0.40, +1) == doctest::Approx(0.5));
    CHECK(*StepScale(0.72, -1) == doctest::Approx(0.5));
    CHECK_FALSE(StepScale(0.72, +1).has_value());
}

TEST_CASE("ResizeViewKeepingAnchor: same top-left and scale, new size") {
    const RECT view = ScaledViewRect(kReal, 0.5, {300, 200});
    const RECT resized = ResizeViewKeepingAnchor(view, 0.5, {1600, 1000});
    CHECK(Same(resized, {300, 200, 1100, 700}));
}

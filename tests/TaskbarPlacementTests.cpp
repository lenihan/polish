#include "windowtracking/TaskbarPlacement.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {

// A 1920 wide, 48 tall bottom taskbar at 100% scale, Start 44 wide, with
// the app buttons packed to its right as Windows lays them out centered.
TaskbarGeometry Centered(LONG startLeft, int appCount) {
    TaskbarGeometry geometry;
    geometry.taskbar = {0, 1032, 1920, 1080};
    geometry.start = RECT{startLeft, 1032, startLeft + 44, 1080};
    LONG x = startLeft + 44;
    for (int i = 0; i < appCount; ++i) {
        geometry.appButtons.push_back({x, 1032, x + 44, 1080});
        x += 44;
    }
    geometry.alignment = TaskbarAlignment::Center;
    geometry.dpi = 96;
    return geometry;
}

bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

TEST_CASE("PolishButtonRect: centered -- immediately left of Start, same size and row") {
    const auto rect = PolishButtonRect(Centered(900, 4));
    REQUIRE(rect.has_value());
    CHECK(Same(*rect, {856, 1032, 900, 1080}));
}

TEST_CASE("PolishButtonRect: centered -- follows Start as the group re-centers") {
    const auto few = PolishButtonRect(Centered(900, 2));
    const auto many = PolishButtonRect(Centered(800, 8));
    REQUIRE(few.has_value());
    REQUIRE(many.has_value());
    CHECK(few->right == 900);
    CHECK(many->right == 800);
}

TEST_CASE("PolishButtonRect: centered, no Start found -- anchors on the group's left edge") {
    TaskbarGeometry geometry = Centered(900, 3);
    geometry.start.reset();
    const auto rect = PolishButtonRect(geometry);
    REQUIRE(rect.has_value());
    // First app button is at 944; one cell to its left.
    CHECK(rect->left == 944 - 44);
}

TEST_CASE("PolishButtonRect: centered, nothing read at all -- falls back to the taskbar's middle") {
    TaskbarGeometry geometry;
    geometry.taskbar = {0, 1032, 1920, 1080};
    const auto rect = PolishButtonRect(geometry);
    REQUIRE(rect.has_value());
    CHECK(rect->right == 960);
    CHECK(rect->right - rect->left == 44);
    CHECK(rect->bottom - rect->top == 48);
}

TEST_CASE("PolishButtonRect: fallback size scales with DPI") {
    TaskbarGeometry geometry;
    geometry.taskbar = {0, 1992, 3840, 2160};
    geometry.dpi = 192;
    const auto rect = PolishButtonRect(geometry);
    REQUIRE(rect.has_value());
    CHECK(rect->right - rect->left == 88);
    CHECK(rect->bottom - rect->top == 96);
}

TEST_CASE("PolishButtonRect: left-aligned -- just after the last app button") {
    TaskbarGeometry geometry = Centered(0, 3);
    geometry.alignment = TaskbarAlignment::Left;
    const auto rect = PolishButtonRect(geometry);
    REQUIRE(rect.has_value());
    // Start 0..44, apps 44..176, button at 176.
    CHECK(rect->left == 176);
    CHECK(rect->right == 220);
}

TEST_CASE("PolishButtonRect: left-aligned with no apps -- right after Start") {
    TaskbarGeometry geometry = Centered(0, 0);
    geometry.alignment = TaskbarAlignment::Left;
    const auto rect = PolishButtonRect(geometry);
    REQUIRE(rect.has_value());
    CHECK(rect->left == 44);
}

TEST_CASE("PolishButtonRect: no room on the left means hidden, not clamped over Start") {
    // Start flush against the left edge on a centered bar: there is no
    // space to its left.
    const auto rect = PolishButtonRect(Centered(0, 2));
    CHECK_FALSE(rect.has_value());
}

TEST_CASE("PolishButtonRect: never overlaps the tray") {
    TaskbarGeometry geometry = Centered(0, 0);
    geometry.alignment = TaskbarAlignment::Left;
    // Enough apps that the next cell would start inside the rightmost
    // quarter (1440+).
    for (LONG x = 44; x < 1440; x += 44) {
        geometry.appButtons.push_back({x, 1032, x + 44, 1080});
    }
    CHECK_FALSE(PolishButtonRect(geometry).has_value());
}

TEST_CASE("PolishButtonRect: an empty taskbar rect has nowhere to put it") {
    TaskbarGeometry geometry;
    CHECK_FALSE(PolishButtonRect(geometry).has_value());
}

TEST_CASE("PolishButtonRect: takes Start's size and row, not the taskbar's") {
    TaskbarGeometry geometry = Centered(900, 2);
    // A Start button 4 px shorter than the taskbar, 2 px in from the top.
    geometry.start = RECT{900, 1034, 948, 1078};
    const auto rect = PolishButtonRect(geometry);
    REQUIRE(rect.has_value());
    CHECK(rect->top == 1034);
    CHECK(rect->bottom == 1078);
    CHECK(rect->right - rect->left == 48);
}

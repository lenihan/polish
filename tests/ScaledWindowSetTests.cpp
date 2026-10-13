#include "windowtracking/ScaledWindowSet.h"

#include "windowtracking/ScaleGeometry.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {

HWND H(int n) {
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(n));
}

constexpr SIZE kReal = {1200, 800};

ScaledEntry MakeEntry(int handle, double scale = 0.5, POINT topLeft = {100, 100}) {
    ScaledEntry entry;
    entry.hwnd = H(handle);
    entry.pid = 4242;
    entry.scale = scale;
    entry.realSize = kReal;
    entry.viewRect = ScaledViewRect(kReal, scale, topLeft);
    return entry;
}

}  // namespace

TEST_CASE("Scale: registers a window once") {
    ScaledWindowSet set;
    CHECK(set.Empty());
    CHECK(set.Scale(MakeEntry(1)));
    CHECK(set.Contains(H(1)));
    CHECK_FALSE(set.Scale(MakeEntry(1)));  // already scaled
    CHECK(set.Entries().size() == 1);
    CHECK_FALSE(set.Scale(ScaledEntry{}));  // null handle
}

TEST_CASE("Unscale: returns the entry and remembers where its view was") {
    ScaledWindowSet set;
    set.Scale(MakeEntry(1, 0.33, {640, 360}));

    const auto removed = set.Unscale(H(1));
    REQUIRE(removed.has_value());
    CHECK(removed->hwnd == H(1));
    CHECK_FALSE(set.Contains(H(1)));

    const auto remembered = set.Remembered(H(1));
    REQUIRE(remembered.has_value());
    CHECK(remembered->scale == doctest::Approx(0.33));
    CHECK(remembered->viewRect.left == 640);
    CHECK(remembered->viewRect.top == 360);

    CHECK_FALSE(set.Unscale(H(1)).has_value());  // second time: nothing to do
}

TEST_CASE("Scale -> Unscale -> Scale: the remembered spot is for the scaling back down") {
    ScaledWindowSet set;
    set.Scale(MakeEntry(1, 0.25, {50, 70}));
    set.Unscale(H(1));
    REQUIRE(set.Remembered(H(1)).has_value());

    // Scaling it again discards the remembered view: the caller has just
    // decided where it goes.
    const RememberedView before = *set.Remembered(H(1));
    set.Scale(MakeEntry(1, before.scale, {before.viewRect.left, before.viewRect.top}));
    CHECK_FALSE(set.Remembered(H(1)).has_value());
    CHECK(set.Find(H(1))->viewRect.left == 50);
    CHECK(set.Find(H(1))->viewRect.top == 70);
}

TEST_CASE("SetViewRect: the scale follows the rect") {
    ScaledWindowSet set;
    set.Scale(MakeEntry(1, 0.5));
    REQUIRE(set.SetViewRect(H(1), ScaledViewRect(kReal, 0.25, {10, 20})));
    CHECK(set.Find(H(1))->scale == doctest::Approx(0.25));
    CHECK_FALSE(set.SetViewRect(H(9), {0, 0, 10, 10}));
}

TEST_CASE("Rescale: keeps the top-left, clamps to the scale bounds") {
    ScaledWindowSet set;
    set.Scale(MakeEntry(1, 0.5, {300, 200}));

    REQUIRE(set.Rescale(H(1), 0.25));
    CHECK(set.Find(H(1))->viewRect.left == 300);
    CHECK(set.Find(H(1))->viewRect.top == 200);
    CHECK(set.Find(H(1))->viewRect.right - set.Find(H(1))->viewRect.left == 300);

    set.Rescale(H(1), 0.001);
    CHECK(set.Find(H(1))->scale == doctest::Approx(kMinScale));
    set.Rescale(H(1), 7.0);
    CHECK(set.Find(H(1))->scale == doctest::Approx(kMaxScale));
}

TEST_CASE("OnRealResized: the view keeps its scale and anchor") {
    ScaledWindowSet set;
    set.Scale(MakeEntry(1, 0.5, {300, 200}));

    REQUIRE(set.OnRealResized(H(1), {1600, 1000}));
    const ScaledEntry* entry = set.Find(H(1));
    CHECK(entry->realSize.cx == 1600);
    CHECK(entry->scale == doctest::Approx(0.5));
    CHECK(entry->viewRect.left == 300);
    CHECK(entry->viewRect.right - entry->viewRect.left == 800);

    CHECK_FALSE(set.OnRealResized(H(1), {0, 0}));  // nonsense size is ignored
}

TEST_CASE("MostRecent: tracks scaling and adjusting") {
    ScaledWindowSet set;
    CHECK(set.MostRecent() == nullptr);

    set.Scale(MakeEntry(1));
    set.Scale(MakeEntry(2));
    set.Scale(MakeEntry(3));
    CHECK(set.MostRecent() == H(3));

    set.Rescale(H(1), 0.25);  // touched -> now the most recent
    CHECK(set.MostRecent() == H(1));

    set.Unscale(H(1));
    CHECK(set.MostRecent() == H(3));
}

TEST_CASE("OnRealDestroyed: forgets the window and its remembered view") {
    ScaledWindowSet set;
    set.Scale(MakeEntry(1));
    set.Scale(MakeEntry(2));
    set.Unscale(H(2));
    REQUIRE(set.Remembered(H(2)).has_value());

    set.OnRealDestroyed(H(1));
    set.OnRealDestroyed(H(2));
    CHECK(set.Empty());
    CHECK_FALSE(set.Remembered(H(2)).has_value());
}

TEST_CASE("SetMinimized: toggles the flag on a scaled window only") {
    ScaledWindowSet set;
    set.Scale(MakeEntry(1));
    CHECK(set.SetMinimized(H(1), true));
    CHECK(set.Find(H(1))->minimized);
    CHECK(set.SetMinimized(H(1), false));
    CHECK_FALSE(set.Find(H(1))->minimized);
    CHECK_FALSE(set.SetMinimized(H(5), true));
}

#include "windowtracking/ArrangeSelection.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using namespace polish;

namespace {

// Stand-in window handles. Never dereferenced -- ArrangeSelection only
// ever compares and orders them.
HWND W(int n) {
    return reinterpret_cast<HWND>(static_cast<INT_PTR>(n));
}

}  // namespace

TEST_CASE("MinimumWindowsFor: each kind needs as many windows as it has slots") {
    CHECK(MinimumWindowsFor(ArrangeKind::TwoWay) == 2);
    CHECK(MinimumWindowsFor(ArrangeKind::ThreeWay) == 3);
    CHECK(MinimumWindowsFor(ArrangeKind::FourWay) == 4);
}

TEST_CASE("EvaluateArrange: available once there are enough windows, and says why when not") {
    CHECK_FALSE(EvaluateArrange(ArrangeKind::TwoWay, 0).enabled);
    CHECK_FALSE(EvaluateArrange(ArrangeKind::TwoWay, 1).enabled);
    CHECK(EvaluateArrange(ArrangeKind::TwoWay, 2).enabled);
    CHECK(EvaluateArrange(ArrangeKind::TwoWay, 7).enabled);

    CHECK_FALSE(EvaluateArrange(ArrangeKind::ThreeWay, 2).enabled);
    CHECK(EvaluateArrange(ArrangeKind::ThreeWay, 3).enabled);

    CHECK_FALSE(EvaluateArrange(ArrangeKind::FourWay, 3).enabled);
    CHECK(EvaluateArrange(ArrangeKind::FourWay, 4).enabled);

    // The reason goes in the menu item's own text, so it has to be both
    // present and short.
    CHECK(std::wstring(EvaluateArrange(ArrangeKind::TwoWay, 1).reason) == L"needs 2 windows");
    CHECK(std::wstring(EvaluateArrange(ArrangeKind::ThreeWay, 1).reason) == L"needs 3 windows");
    CHECK(std::wstring(EvaluateArrange(ArrangeKind::FourWay, 1).reason) == L"needs 4 windows");
    CHECK(std::wstring(EvaluateArrange(ArrangeKind::TwoWay, 2).reason).empty());
}

TEST_CASE("ArrangeToggle: two-way is a swap, and the third press comes back") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2)};

    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(2), W(1)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(2), W(1)});
}

TEST_CASE("ArrangeToggle: three-way gives every window the first slot in three presses") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3)};

    // Each repeat moves every window one slot along, the last wrapping to
    // the first.
    const auto first = toggle.Next(ArrangeKind::ThreeWay, mru);
    const auto second = toggle.Next(ArrangeKind::ThreeWay, mru);
    const auto third = toggle.Next(ArrangeKind::ThreeWay, mru);
    CHECK(first == std::vector<HWND>{W(1), W(2), W(3)});
    CHECK(second == std::vector<HWND>{W(3), W(1), W(2)});
    CHECK(third == std::vector<HWND>{W(2), W(3), W(1)});

    // Each window led exactly once.
    CHECK(first[0] != second[0]);
    CHECK(second[0] != third[0]);
    CHECK(first[0] != third[0]);

    // And the fourth press is the first again.
    CHECK(toggle.Next(ArrangeKind::ThreeWay, mru) == first);
}

TEST_CASE("ArrangeToggle: four-way gives every window the first slot in four presses") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3), W(4)};

    std::vector<HWND> leaders;
    std::vector<std::vector<HWND>> seen;
    for (int i = 0; i < 4; ++i) {
        seen.push_back(toggle.Next(ArrangeKind::FourWay, mru));
        leaders.push_back(seen.back()[0]);
    }
    CHECK(seen[0] == std::vector<HWND>{W(1), W(2), W(3), W(4)});
    CHECK(seen[1] == std::vector<HWND>{W(4), W(1), W(2), W(3)});
    CHECK(seen[2] == std::vector<HWND>{W(3), W(4), W(1), W(2)});
    CHECK(seen[3] == std::vector<HWND>{W(2), W(3), W(4), W(1)});
    CHECK(leaders == std::vector<HWND>{W(1), W(4), W(3), W(2)});

    // The fourth repeat comes round to the start.
    CHECK(toggle.Next(ArrangeKind::FourWay, mru) == seen[0]);
}

TEST_CASE("ArrangeToggle: only the most recent windows are touched") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3), W(4), W(5), W(6)};

    for (int i = 0; i < 5; ++i) {
        const auto chosen = toggle.Next(ArrangeKind::FourWay, mru);
        REQUIRE(chosen.size() == 4);
        // The two oldest windows are left alone, however many presses.
        CHECK(std::find(chosen.begin(), chosen.end(), W(5)) == chosen.end());
        CHECK(std::find(chosen.begin(), chosen.end(), W(6)) == chosen.end());
    }
}

TEST_CASE("ArrangeToggle: not enough windows gives nothing back and does not advance anything") {
    ArrangeToggle toggle;
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1)}).empty());
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(1), W(2)}).empty());
    CHECK(toggle.Next(ArrangeKind::FourWay, {W(1), W(2), W(3)}).empty());

    // A refused invocation must not have consumed the rotation, or the
    // first press that does work would arrive already shifted.
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(2)}) == std::vector<HWND>{W(1), W(2)});
}

TEST_CASE("ArrangeToggle: a different set of windows starts again in MRU order") {
    ArrangeToggle toggle;
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(2)}) == std::vector<HWND>{W(1), W(2)});
    // Rotated now.
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(2)}) == std::vector<HWND>{W(2), W(1)});

    // One of them closed and another took its place: a new question, so
    // the answer starts from MRU order rather than inheriting the rotation.
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(3)}) == std::vector<HWND>{W(1), W(3)});
}

TEST_CASE("ArrangeToggle: merely focusing something else does not reset the rotation") {
    // The failure this pins: keying the toggle on MRU *order* rather than
    // on the set would reset it almost every time, because focusing a
    // window between two presses reorders the list. The rotation would then
    // look stuck in exactly the situation it is for.
    ArrangeToggle toggle;
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(1), W(2), W(3)}) == std::vector<HWND>{W(1), W(2), W(3)});

    // Same three windows, different MRU order. Still rotates, and rotates
    // the order as it is now rather than the one seen last time.
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(2), W(1), W(3)}) == std::vector<HWND>{W(3), W(2), W(1)});
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(2), W(1), W(3)}) == std::vector<HWND>{W(1), W(3), W(2)});
}

TEST_CASE("ArrangeToggle: the kinds keep their own state") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3), W(4)};

    // Advance 2-way once.
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(2), W(1)});

    // 4-way has not been used yet, so it starts in MRU order rather than
    // inheriting 2-way's rotation.
    CHECK(toggle.Next(ArrangeKind::FourWay, mru) == std::vector<HWND>{W(1), W(2), W(3), W(4)});

    // And using 4-way did not disturb where 2-way had got to.
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
}

TEST_CASE("ArrangeToggle: Reset puts every kind back to MRU order") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3)};
    CHECK(toggle.Next(ArrangeKind::ThreeWay, mru) == std::vector<HWND>{W(1), W(2), W(3)});
    CHECK(toggle.Next(ArrangeKind::ThreeWay, mru) == std::vector<HWND>{W(3), W(1), W(2)});
    toggle.Reset();
    CHECK(toggle.Next(ArrangeKind::ThreeWay, mru) == std::vector<HWND>{W(1), W(2), W(3)});
}

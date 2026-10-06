#include "windowtracking/ArrangeSelection.h"

#include <doctest/doctest.h>

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

TEST_CASE("ArrangeToggle: the second press reverses and the third comes back") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2)};

    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(2), W(1)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(2), W(1)});
}

TEST_CASE("ArrangeToggle: only the most recent windows are touched") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3), W(4), W(5), W(6)};

    const auto first = toggle.Next(ArrangeKind::FourWay, mru);
    CHECK(first == std::vector<HWND>{W(1), W(2), W(3), W(4)});

    const auto second = toggle.Next(ArrangeKind::FourWay, mru);
    CHECK(second == std::vector<HWND>{W(4), W(3), W(2), W(1)});

    // The two oldest windows are left alone entirely, in both directions.
    for (const auto& chosen : {first, second}) {
        CHECK(std::find(chosen.begin(), chosen.end(), W(5)) == chosen.end());
        CHECK(std::find(chosen.begin(), chosen.end(), W(6)) == chosen.end());
    }
}

TEST_CASE("ArrangeToggle: three-way takes three and reverses them") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3)};
    CHECK(toggle.Next(ArrangeKind::ThreeWay, mru) == std::vector<HWND>{W(1), W(2), W(3)});
    CHECK(toggle.Next(ArrangeKind::ThreeWay, mru) == std::vector<HWND>{W(3), W(2), W(1)});
}

TEST_CASE("ArrangeToggle: not enough windows gives nothing back and does not flip anything") {
    ArrangeToggle toggle;
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1)}).empty());
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(1), W(2)}).empty());
    CHECK(toggle.Next(ArrangeKind::FourWay, {W(1), W(2), W(3)}).empty());

    // A refused invocation must not have consumed the toggle, or the first
    // press that does work would arrive already reversed.
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(2)}) == std::vector<HWND>{W(1), W(2)});
}

TEST_CASE("ArrangeToggle: a different set of windows starts again in MRU order") {
    ArrangeToggle toggle;
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(2)}) == std::vector<HWND>{W(1), W(2)});
    // Flipped now.
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(2)}) == std::vector<HWND>{W(2), W(1)});

    // One of them closed and another took its place: a new question, so
    // the answer starts from MRU order rather than inheriting the flip.
    CHECK(toggle.Next(ArrangeKind::TwoWay, {W(1), W(3)}) == std::vector<HWND>{W(1), W(3)});
}

TEST_CASE("ArrangeToggle: merely focusing something else does not reset the toggle") {
    // The failure this pins: keying the toggle on MRU *order* rather than
    // on the set would reset it almost every time, because clicking either
    // window between two presses reorders the list. The reversal would
    // then look broken in exactly the situation it is for.
    ArrangeToggle toggle;
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(1), W(2), W(3)}) == std::vector<HWND>{W(1), W(2), W(3)});

    // Same three windows, different MRU order. Still flips -- and flips
    // the order as it is now, not the one it saw last time.
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(2), W(1), W(3)}) == std::vector<HWND>{W(3), W(1), W(2)});
    CHECK(toggle.Next(ArrangeKind::ThreeWay, {W(2), W(1), W(3)}) == std::vector<HWND>{W(2), W(1), W(3)});
}

TEST_CASE("ArrangeToggle: the kinds keep their own state") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2), W(3), W(4)};

    // Flip 2-way.
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(2), W(1)});

    // 4-way has not been used yet, so it starts in MRU order rather than
    // inheriting 2-way's flip.
    CHECK(toggle.Next(ArrangeKind::FourWay, mru) == std::vector<HWND>{W(1), W(2), W(3), W(4)});

    // And using 4-way did not disturb where 2-way had got to.
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
}

TEST_CASE("ArrangeToggle: Reset puts every kind back to MRU order") {
    ArrangeToggle toggle;
    const std::vector<HWND> mru{W(1), W(2)};
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
    toggle.Reset();
    CHECK(toggle.Next(ArrangeKind::TwoWay, mru) == std::vector<HWND>{W(1), W(2)});
}

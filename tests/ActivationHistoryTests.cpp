#include "windowtracking/ActivationHistory.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {
// Real HWNDs are opaque pointers; these tests never dereference them, so
// arbitrary distinct values stand in fine.
HWND AsHwnd(uintptr_t value) { return reinterpret_cast<HWND>(value); }
}  // namespace

TEST_CASE("ActivationHistory: empty history has no ordered windows") {
    ActivationHistory history;
    CHECK(history.OrderedWindows().empty());
}

TEST_CASE("ActivationHistory: MoveToFront on an empty history adds the window") {
    ActivationHistory history;
    history.MoveToFront(AsHwnd(1));
    CHECK(history.OrderedWindows() == std::vector<HWND>{AsHwnd(1)});
}

TEST_CASE("ActivationHistory: repeated activations produce most-recent-first order") {
    ActivationHistory history;
    history.MoveToFront(AsHwnd(1));
    history.MoveToFront(AsHwnd(2));
    history.MoveToFront(AsHwnd(3));
    CHECK(history.OrderedWindows() == std::vector<HWND>{AsHwnd(3), AsHwnd(2), AsHwnd(1)});
}

TEST_CASE("ActivationHistory: re-activating an existing window moves it to front without duplicating") {
    ActivationHistory history;
    history.MoveToFront(AsHwnd(1));
    history.MoveToFront(AsHwnd(2));
    history.MoveToFront(AsHwnd(3));
    history.MoveToFront(AsHwnd(1));  // re-activate the oldest
    CHECK(history.OrderedWindows() == std::vector<HWND>{AsHwnd(1), AsHwnd(3), AsHwnd(2)});
}

TEST_CASE("ActivationHistory: Remove drops a window and preserves the order of the rest") {
    ActivationHistory history;
    history.MoveToFront(AsHwnd(1));
    history.MoveToFront(AsHwnd(2));
    history.MoveToFront(AsHwnd(3));
    history.Remove(AsHwnd(2));
    CHECK(history.OrderedWindows() == std::vector<HWND>{AsHwnd(3), AsHwnd(1)});
}

TEST_CASE("ActivationHistory: Remove of a window not in the history is a no-op") {
    ActivationHistory history;
    history.MoveToFront(AsHwnd(1));
    history.Remove(AsHwnd(99));
    CHECK(history.OrderedWindows() == std::vector<HWND>{AsHwnd(1)});
}

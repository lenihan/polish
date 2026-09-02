#include "windowtracking/GroupState.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {
// Real HWNDs are opaque pointers; these tests never dereference them, so
// arbitrary distinct values stand in fine.
HWND AsHwnd(uintptr_t value) { return reinterpret_cast<HWND>(value); }
}  // namespace

TEST_CASE("GroupState: new group has an id, default Tab mode, and no members") {
    GroupState state(42);
    CHECK(state.Id() == 42);
    CHECK(state.Mode() == GroupMode::Tab);
    CHECK(state.MemberCount() == 0);
    CHECK(state.ActiveIndex() == std::nullopt);
    CHECK(state.ActiveWindow() == std::nullopt);
}

TEST_CASE("GroupState: new group defaults to Horizontal alignment and an auto-generated name") {
    GroupState state(42);
    CHECK(state.Alignment() == GroupAlignment::Horizontal);
    CHECK(state.Name() == L"Group 42");
}

TEST_CASE("GroupState: mode can be set at construction") {
    GroupState state(1, GroupMode::Tile);
    CHECK(state.Mode() == GroupMode::Tile);
}

TEST_CASE("GroupState: AddWindow appends and the first window becomes active") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    CHECK(state.MemberCount() == 1);
    CHECK(state.Contains(AsHwnd(1)));
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("GroupState: AddWindow of a second window doesn't change which is active") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    CHECK(state.MemberCount() == 2);
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("GroupState: AddWindow of an already-present window is a no-op") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(1));  // already present
    CHECK(state.MemberCount() == 2);
}

TEST_CASE("GroupState: SetActiveIndex changes the active member") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.SetActiveIndex(1);
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(2));
}

TEST_CASE("GroupState: SetActiveIndex out of range is a no-op") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetActiveIndex(5);
    CHECK(state.ActiveIndex() == 0);
}

TEST_CASE("GroupState: SetActiveWindow finds and activates the matching member") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveWindow(AsHwnd(3));
    CHECK(state.ActiveIndex() == 2);
    CHECK(state.ActiveWindow() == AsHwnd(3));
}

TEST_CASE("GroupState: SetActiveWindow of a non-member is a no-op") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetActiveWindow(AsHwnd(99));
    CHECK(state.ActiveIndex() == 0);
}

TEST_CASE("GroupState: Remove of a non-active member shifts a later active index down") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(2);  // AsHwnd(3) is active
    state.Remove(AsHwnd(1));  // removed before the active index
    CHECK_FALSE(state.Contains(AsHwnd(1)));
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(3));
}

TEST_CASE("GroupState: Remove of a non-active member after the active index doesn't move it") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    // AsHwnd(1) (index 0) stays active.
    state.Remove(AsHwnd(3));
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("GroupState: removing the active member (not last) promotes whatever shifted into its slot") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(1);  // AsHwnd(2) is active
    state.Remove(AsHwnd(2));
    CHECK(state.MemberCount() == 2);
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(3));
}

TEST_CASE("GroupState: removing the active member when it's last falls back to the new last member") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(2);  // AsHwnd(3), the last one, is active
    state.Remove(AsHwnd(3));
    CHECK(state.MemberCount() == 2);
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(2));
}

TEST_CASE("GroupState: removing the only member leaves no active member") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.Remove(AsHwnd(1));
    CHECK(state.MemberCount() == 0);
    CHECK(state.ActiveIndex() == std::nullopt);
    CHECK(state.ActiveWindow() == std::nullopt);
}

TEST_CASE("GroupState: Remove of a window not in the group is a no-op") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.Remove(AsHwnd(99));
    CHECK(state.MemberCount() == 1);
    CHECK(state.Contains(AsHwnd(1)));
}

TEST_CASE("GroupState: SetMode changes the mode without touching membership") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetMode(GroupMode::Tile);
    CHECK(state.Mode() == GroupMode::Tile);
    CHECK(state.MemberCount() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("GroupState: SetAlignment changes the alignment without touching membership") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetAlignment(GroupAlignment::Vertical);
    CHECK(state.Alignment() == GroupAlignment::Vertical);
    CHECK(state.MemberCount() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("GroupState: SetName replaces the auto-generated name") {
    GroupState state(1);
    state.SetName(L"projA");
    CHECK(state.Name() == L"projA");
}

TEST_CASE("GroupState: Reorder moves a member forward, shifting others back") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.Reorder(0, 2);  // move AsHwnd(1) to the end
    REQUIRE(state.Members().size() == 3);
    CHECK(state.Members()[0].window == AsHwnd(2));
    CHECK(state.Members()[1].window == AsHwnd(3));
    CHECK(state.Members()[2].window == AsHwnd(1));
}

TEST_CASE("GroupState: Reorder moves a member backward, shifting others forward") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.Reorder(2, 0);  // move AsHwnd(3) to the front
    REQUIRE(state.Members().size() == 3);
    CHECK(state.Members()[0].window == AsHwnd(3));
    CHECK(state.Members()[1].window == AsHwnd(1));
    CHECK(state.Members()[2].window == AsHwnd(2));
}

TEST_CASE("GroupState: Reorder keeps the active member correctly tracked when it's the one moved") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(0);  // AsHwnd(1) is active
    state.Reorder(0, 2);      // AsHwnd(1) moves to the end
    CHECK(state.ActiveIndex() == 2);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("GroupState: Reorder keeps the active member correctly tracked when a different member moves") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(1);  // AsHwnd(2) is active
    state.Reorder(0, 2);      // AsHwnd(1) moves to the end; AsHwnd(2) shifts to index 0
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(2));
}

TEST_CASE("GroupState: Reorder with equal indices is a no-op") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.Reorder(0, 0);
    CHECK(state.Members()[0].window == AsHwnd(1));
    CHECK(state.Members()[1].window == AsHwnd(2));
}

TEST_CASE("GroupState: Reorder with an out-of-range index is a no-op") {
    GroupState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.Reorder(0, 5);
    CHECK(state.Members()[0].window == AsHwnd(1));
    CHECK(state.Members()[1].window == AsHwnd(2));
}

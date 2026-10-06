#include "windowtracking/StackState.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {
// Real HWNDs are opaque pointers; these tests never dereference them, so
// arbitrary distinct values stand in fine.
HWND AsHwnd(uintptr_t value) { return reinterpret_cast<HWND>(value); }
}  // namespace

TEST_CASE("StackState: new stack has an id and no members") {
    StackState state(42);
    CHECK(state.Id() == 42);
    CHECK(state.MemberCount() == 0);
    CHECK(state.ActiveIndex() == std::nullopt);
    CHECK(state.ActiveWindow() == std::nullopt);
}

TEST_CASE("StackState: new stack defaults to Horizontal alignment and an auto-generated name") {
    StackState state(42);
    CHECK(state.Alignment() == StackAlignment::Horizontal);
    CHECK(state.Name() == L"Stack 42");
}

TEST_CASE("StackState: AddWindow appends and the first window becomes active") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    CHECK(state.MemberCount() == 1);
    CHECK(state.Contains(AsHwnd(1)));
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("StackState: AddWindow of a second window doesn't change which is active") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    CHECK(state.MemberCount() == 2);
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("StackState: AddWindow of an already-present window is a no-op") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(1));  // already present
    CHECK(state.MemberCount() == 2);
}

TEST_CASE("StackState: SetActiveIndex changes the active member") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.SetActiveIndex(1);
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(2));
}

TEST_CASE("StackState: SetActiveIndex out of range is a no-op") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetActiveIndex(5);
    CHECK(state.ActiveIndex() == 0);
}

TEST_CASE("StackState: SetActiveWindow finds and activates the matching member") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveWindow(AsHwnd(3));
    CHECK(state.ActiveIndex() == 2);
    CHECK(state.ActiveWindow() == AsHwnd(3));
}

TEST_CASE("StackState: SetActiveWindow of a non-member is a no-op") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetActiveWindow(AsHwnd(99));
    CHECK(state.ActiveIndex() == 0);
}

TEST_CASE("StackState: Remove of a non-active member shifts a later active index down") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(2);  // AsHwnd(3) is active
    state.Remove(AsHwnd(1));  // removed before the active index
    CHECK_FALSE(state.Contains(AsHwnd(1)));
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(3));
}

TEST_CASE("StackState: Remove of a non-active member after the active index doesn't move it") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    // AsHwnd(1) (index 0) stays active.
    state.Remove(AsHwnd(3));
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("StackState: removing the active member (not last) promotes whatever shifted into its slot") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(1);  // AsHwnd(2) is active
    state.Remove(AsHwnd(2));
    CHECK(state.MemberCount() == 2);
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(3));
}

TEST_CASE("StackState: removing the active member when it's last falls back to the new last member") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(2);  // AsHwnd(3), the last one, is active
    state.Remove(AsHwnd(3));
    CHECK(state.MemberCount() == 2);
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(2));
}

TEST_CASE("StackState: removing the only member leaves no active member") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.Remove(AsHwnd(1));
    CHECK(state.MemberCount() == 0);
    CHECK(state.ActiveIndex() == std::nullopt);
    CHECK(state.ActiveWindow() == std::nullopt);
}

TEST_CASE("StackState: Remove of a window not in the stack is a no-op") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.Remove(AsHwnd(99));
    CHECK(state.MemberCount() == 1);
    CHECK(state.Contains(AsHwnd(1)));
}

TEST_CASE("StackState: SetAlignment changes the alignment without touching membership") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetAlignment(StackAlignment::Vertical);
    CHECK(state.Alignment() == StackAlignment::Vertical);
    CHECK(state.MemberCount() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("StackState: SetName replaces the auto-generated name") {
    StackState state(1);
    state.SetName(L"projA");
    CHECK(state.Name() == L"projA");
}

TEST_CASE("StackState: Reorder moves a member forward, shifting others back") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.Reorder(0, 2);  // move AsHwnd(1) to the end
    REQUIRE(state.Members().size() == 3);
    CHECK(state.Members()[0].window == AsHwnd(2));
    CHECK(state.Members()[1].window == AsHwnd(3));
    CHECK(state.Members()[2].window == AsHwnd(1));
}

TEST_CASE("StackState: Reorder moves a member backward, shifting others forward") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.Reorder(2, 0);  // move AsHwnd(3) to the front
    REQUIRE(state.Members().size() == 3);
    CHECK(state.Members()[0].window == AsHwnd(3));
    CHECK(state.Members()[1].window == AsHwnd(1));
    CHECK(state.Members()[2].window == AsHwnd(2));
}

TEST_CASE("StackState: Reorder keeps the active member correctly tracked when it's the one moved") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(0);  // AsHwnd(1) is active
    state.Reorder(0, 2);      // AsHwnd(1) moves to the end
    CHECK(state.ActiveIndex() == 2);
    CHECK(state.ActiveWindow() == AsHwnd(1));
}

TEST_CASE("StackState: Reorder keeps the active member correctly tracked when a different member moves") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.AddWindow(AsHwnd(3));
    state.SetActiveIndex(1);  // AsHwnd(2) is active
    state.Reorder(0, 2);      // AsHwnd(1) moves to the end; AsHwnd(2) shifts to index 0
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(2));
}

TEST_CASE("StackState: Reorder with equal indices is a no-op") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.Reorder(0, 0);
    CHECK(state.Members()[0].window == AsHwnd(1));
    CHECK(state.Members()[1].window == AsHwnd(2));
}

TEST_CASE("StackState: Reorder with an out-of-range index is a no-op") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.Reorder(0, 5);
    CHECK(state.Members()[0].window == AsHwnd(1));
    CHECK(state.Members()[1].window == AsHwnd(2));
}

TEST_CASE("StackState: SetMembers replaces membership wholesale, dropping and adding as needed") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.SetMembers({AsHwnd(2), AsHwnd(3)});  // 1 dropped, 2 kept, 3 added
    CHECK(state.MemberCount() == 2);
    CHECK(state.Members()[0].window == AsHwnd(2));
    CHECK(state.Members()[1].window == AsHwnd(3));
}

TEST_CASE("StackState: SetMembers keeps the previously active member active by identity") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.SetActiveIndex(1);  // AsHwnd(2) is active
    state.SetMembers({AsHwnd(3), AsHwnd(2), AsHwnd(1)});
    CHECK(state.ActiveIndex() == 1);
    CHECK(state.ActiveWindow() == AsHwnd(2));
}

TEST_CASE("StackState: SetMembers falls back to the first member when the active one is dropped") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.SetActiveIndex(1);  // AsHwnd(2) is active
    state.SetMembers({AsHwnd(3), AsHwnd(4)});  // AsHwnd(2) no longer present
    CHECK(state.ActiveIndex() == 0);
    CHECK(state.ActiveWindow() == AsHwnd(3));
}

TEST_CASE("StackState: SetMembers with an empty list clears membership and the active member") {
    StackState state(1);
    state.AddWindow(AsHwnd(1));
    state.SetMembers({});
    CHECK(state.MemberCount() == 0);
    CHECK(state.ActiveIndex() == std::nullopt);
    CHECK(state.ActiveWindow() == std::nullopt);
}

TEST_CASE("StackState: a new stack has no rect until it is placed") {
    StackState state(1);
    CHECK_FALSE(state.HasRect());
    state.SetRect(RECT{100, 200, 1300, 900});
    CHECK(state.HasRect());
    CHECK(state.Rect().left == 100);
    CHECK(state.Rect().bottom == 900);
}

TEST_CASE("StackState: Offset slides the rect without resizing it") {
    StackState state(1);
    state.SetRect(RECT{100, 200, 1300, 900});
    state.Offset(50, -20);
    CHECK(state.Rect().left == 150);
    CHECK(state.Rect().top == 180);
    CHECK(state.Rect().right - state.Rect().left == 1200);
    CHECK(state.Rect().bottom - state.Rect().top == 700);
}

TEST_CASE("StackState: the rect is independent of membership") {
    StackState state(1);
    state.SetRect(RECT{0, 0, 800, 600});
    state.AddWindow(AsHwnd(1));
    state.AddWindow(AsHwnd(2));
    state.Remove(AsHwnd(1));
    CHECK(state.Rect().right == 800);
}

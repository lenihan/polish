#include "windowtracking/StackManager.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {
HWND AsHwnd(uintptr_t value) { return reinterpret_cast<HWND>(value); }
}  // namespace

TEST_CASE("StackManager: CreateStack assigns increasing ids starting at 1") {
    StackManager manager;
    const StackId first = manager.CreateStack({});
    const StackId second = manager.CreateStack({});
    CHECK(first == 1);
    CHECK(second == 2);
    CHECK(manager.StackCount() == 2);
}

TEST_CASE("StackManager: CreateStack populates membership in order") {
    StackManager manager;
    const StackId id = manager.CreateStack({AsHwnd(1), AsHwnd(2), AsHwnd(3)});
    StackState* stack = manager.FindStack(id);
    REQUIRE(stack != nullptr);
    CHECK(stack->MemberCount() == 3);
    CHECK(stack->Contains(AsHwnd(1)));
    CHECK(stack->Contains(AsHwnd(2)));
    CHECK(stack->Contains(AsHwnd(3)));
    CHECK(stack->ActiveWindow() == AsHwnd(1));
}

TEST_CASE("StackManager: FindStack returns nullptr for an unknown id") {
    StackManager manager;
    manager.CreateStack({});
    CHECK(manager.FindStack(999) == nullptr);
}

TEST_CASE("StackManager: RemoveStack forgets the stack and leaves the others alone") {
    StackManager manager;
    const StackId first = manager.CreateStack({AsHwnd(1), AsHwnd(2)});
    const StackId second = manager.CreateStack({AsHwnd(3), AsHwnd(4)});
    manager.RemoveStack(first);
    CHECK(manager.StackCount() == 1);
    CHECK(manager.FindStack(first) == nullptr);
    REQUIRE(manager.FindStack(second) != nullptr);
    CHECK(manager.FindStack(second)->Contains(AsHwnd(3)));
}

TEST_CASE("StackManager: RemoveStack of an unknown id is harmless") {
    StackManager manager;
    manager.CreateStack({AsHwnd(1)});
    manager.RemoveStack(42);
    CHECK(manager.StackCount() == 1);
}

TEST_CASE("StackManager: ids are not reused after a stack is removed") {
    StackManager manager;
    const StackId first = manager.CreateStack({});
    manager.RemoveStack(first);
    const StackId second = manager.CreateStack({});
    CHECK(second != first);
}

TEST_CASE("StackManager: FindStackContaining finds the stack a window belongs to") {
    StackManager manager;
    const StackId a = manager.CreateStack({AsHwnd(1), AsHwnd(2)});
    const StackId b = manager.CreateStack({AsHwnd(3)});
    REQUIRE(manager.FindStackContaining(AsHwnd(2)) != nullptr);
    CHECK(manager.FindStackContaining(AsHwnd(2))->Id() == a);
    REQUIRE(manager.FindStackContaining(AsHwnd(3)) != nullptr);
    CHECK(manager.FindStackContaining(AsHwnd(3))->Id() == b);
    CHECK(manager.FindStackContaining(AsHwnd(99)) == nullptr);
}

TEST_CASE("StackManager: a removed window is no longer found in its stack") {
    StackManager manager;
    const StackId id = manager.CreateStack({AsHwnd(1), AsHwnd(2), AsHwnd(3)});
    manager.FindStack(id)->Remove(AsHwnd(2));
    CHECK(manager.FindStackContaining(AsHwnd(2)) == nullptr);
    CHECK(manager.FindStack(id)->MemberCount() == 2);
}

TEST_CASE("StackManager: MinimumContentSize ignores windows that do not exist") {
    // Fake handles are not windows, so they impose no minimum -- and must not
    // be asked anything, which for a real stale handle could block.
    StackManager manager;
    const StackId id = manager.CreateStack({AsHwnd(0x1000), AsHwnd(0x2000)});
    const SIZE minimum = manager.MinimumContentSize(*manager.FindStack(id));
    CHECK(minimum.cx == 0);
    CHECK(minimum.cy == 0);
}

TEST_CASE("StackManager: placing members that do not exist is a safe no-op") {
    StackManager manager;
    const StackId id = manager.CreateStack({AsHwnd(0x1000), AsHwnd(0x2000)});
    manager.PlaceMembers(*manager.FindStack(id), RECT{0, 0, 800, 600});
    manager.RaiseActive(*manager.FindStack(id));
    CHECK(manager.StackCount() == 1);
}

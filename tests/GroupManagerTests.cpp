#include "windowtracking/GroupManager.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {
HWND AsHwnd(uintptr_t value) { return reinterpret_cast<HWND>(value); }
}  // namespace

TEST_CASE("GroupManager: CreateGroup assigns increasing ids starting at 1") {
    GroupManager manager;
    const GroupId first = manager.CreateGroup({});
    const GroupId second = manager.CreateGroup({});
    CHECK(first == 1);
    CHECK(second == 2);
    CHECK(manager.GroupCount() == 2);
}

TEST_CASE("GroupManager: CreateGroup populates membership in order") {
    GroupManager manager;
    const GroupId id = manager.CreateGroup({AsHwnd(1), AsHwnd(2), AsHwnd(3)});
    GroupState* group = manager.FindGroup(id);
    REQUIRE(group != nullptr);
    CHECK(group->MemberCount() == 3);
    CHECK(group->Contains(AsHwnd(1)));
    CHECK(group->Contains(AsHwnd(2)));
    CHECK(group->Contains(AsHwnd(3)));
    CHECK(group->ActiveWindow() == AsHwnd(1));
}

TEST_CASE("GroupManager: CreateGroup defaults to Tab mode") {
    GroupManager manager;
    const GroupId id = manager.CreateGroup({AsHwnd(1)});
    GroupState* group = manager.FindGroup(id);
    REQUIRE(group != nullptr);
    CHECK(group->Mode() == GroupMode::Tab);
}

TEST_CASE("GroupManager: CreateGroup can be given Tile mode explicitly") {
    GroupManager manager;
    const GroupId id = manager.CreateGroup({AsHwnd(1)}, GroupMode::Tile);
    GroupState* group = manager.FindGroup(id);
    REQUIRE(group != nullptr);
    CHECK(group->Mode() == GroupMode::Tile);
}

TEST_CASE("GroupManager: FindGroup returns nullptr for an unknown id") {
    GroupManager manager;
    manager.CreateGroup({});
    CHECK(manager.FindGroup(999) == nullptr);
}

#include "windowtracking/StackCollapse.h"

#include <doctest/doctest.h>

using namespace polish;

namespace {

HWND W(int n) {
    return reinterpret_cast<HWND>(static_cast<INT_PTR>(n));
}

StackEntry Stack(std::vector<int> members, int representative) {
    StackEntry s;
    for (int m : members) {
        s.members.push_back(W(m));
    }
    s.representative = W(representative);
    return s;
}

std::vector<HWND> List(std::vector<int> items) {
    std::vector<HWND> out;
    for (int i : items) {
        out.push_back(W(i));
    }
    return out;
}

}  // namespace

TEST_CASE("CollapseStackMembers: with no stacks the list is unchanged") {
    CHECK(CollapseStackMembers(List({1, 2, 3}), {}) == List({1, 2, 3}));
}

TEST_CASE("CollapseStackMembers: non-members keep their relative order exactly") {
    const auto result = CollapseStackMembers(List({1, 10, 2, 11, 3, 12}), {Stack({10, 11, 12}, 10)});
    // The stack folds to one entry at its earliest member's position; the
    // others stay in the order they were in.
    CHECK(result == List({1, 10, 2, 3}));
}

TEST_CASE("CollapseStackMembers: the entry sits at the earliest member's position, not the active one's") {
    // Member 12 is active, but 10 turns up first. If the entry were placed
    // by the active member, switching tabs inside the stack would reorder
    // the whole list.
    const auto result = CollapseStackMembers(List({1, 10, 2, 12, 3}), {Stack({10, 11, 12}, 12)});
    CHECK(result == List({1, 12, 2, 3}));
}

TEST_CASE("CollapseStackMembers: the position does not move when the active tab changes") {
    const std::vector<HWND> ordered = List({1, 10, 2, 11, 3});
    const auto activeTen = CollapseStackMembers(ordered, {Stack({10, 11}, 10)});
    const auto activeEleven = CollapseStackMembers(ordered, {Stack({10, 11}, 11)});
    REQUIRE(activeTen.size() == activeEleven.size());
    // Same slot in the list either way; only which window it is differs.
    CHECK(activeTen == List({1, 10, 2, 3}));
    CHECK(activeEleven == List({1, 11, 2, 3}));
}

TEST_CASE("CollapseStackMembers: the entry is the active member") {
    const auto result = CollapseStackMembers(List({10, 11, 12}), {Stack({10, 11, 12}, 11)});
    CHECK(result == List({11}));
}

TEST_CASE("CollapseStackMembers: an active member that is not in the list falls back to the first one that is") {
    // 12 is the active tab but is minimized, so it is not in this list.
    const auto result = CollapseStackMembers(List({1, 10, 11}), {Stack({10, 11, 12}, 12)});
    CHECK(result == List({1, 10}));
}

TEST_CASE("CollapseStackMembers: a stack with no member in the list contributes nothing") {
    CHECK(CollapseStackMembers(List({1, 2}), {Stack({10, 11}, 10)}) == List({1, 2}));
}

TEST_CASE("CollapseStackMembers: two stacks each fold at their own first member") {
    const auto result =
        CollapseStackMembers(List({10, 20, 11, 21, 5}), {Stack({10, 11}, 10), Stack({20, 21}, 21)});
    CHECK(result == List({10, 21, 5}));
}

TEST_CASE("CollapseStackMembers: a one-member stack still gives one entry and does not crash") {
    CHECK(CollapseStackMembers(List({1, 10, 2}), {Stack({10}, 10)}) == List({1, 10, 2}));
}

TEST_CASE("CollapseStackMembers: an empty list stays empty") {
    CHECK(CollapseStackMembers({}, {Stack({10, 11}, 10)}).empty());
}

TEST_CASE("StackIndexOf: finds the stack a window is in, or -1") {
    const std::vector<StackEntry> stacks = {Stack({10, 11}, 10), Stack({20}, 20)};
    CHECK(StackIndexOf(stacks, W(11)) == 0);
    CHECK(StackIndexOf(stacks, W(20)) == 1);
    CHECK(StackIndexOf(stacks, W(99)) == -1);
}

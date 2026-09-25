#include "windowtracking/TaskbarReadPolicy.h"

#include <doctest/doctest.h>

using namespace polish;

TEST_CASE("DecideOnRead: a good read applies and clears the bad-read count") {
    ReadPolicyState state;
    state.consecutiveBadReads = 2;
    CHECK(DecideOnRead(state, /*usable=*/true, /*buttonCount=*/7, /*haveLastGood=*/true) == ReadDecision::Apply);
    CHECK(state.consecutiveBadReads == 0);
}

TEST_CASE("DecideOnRead: one failed read keeps the shield rather than uncovering it") {
    // The whole point: a transient failure must not open the strip.
    ReadPolicyState state;
    CHECK(DecideOnRead(state, /*usable=*/false, 0, /*haveLastGood=*/true) == ReadDecision::KeepLastGood);
    CHECK(state.consecutiveBadReads == 1);
}

TEST_CASE("DecideOnRead: a usable but empty read is doubted while there is a picture to keep") {
    // UI Automation can return a task-button-less tree mid-relayout, which
    // a single sample cannot tell apart from a taskbar with no apps.
    ReadPolicyState state;
    CHECK(DecideOnRead(state, /*usable=*/true, /*buttonCount=*/0, /*haveLastGood=*/true) ==
          ReadDecision::KeepLastGood);
}

TEST_CASE("DecideOnRead: degrades only after enough consecutive bad reads") {
    ReadPolicyState state;
    for (int i = 1; i < kBadReadsBeforeDegrade; ++i) {
        CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::KeepLastGood);
    }
    CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::Degrade);
}

TEST_CASE("DecideOnRead: a good read in the middle resets the count") {
    ReadPolicyState state;
    CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::KeepLastGood);
    CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::KeepLastGood);
    CHECK(DecideOnRead(state, true, 5, true) == ReadDecision::Apply);
    // Back to needing the full run of failures.
    CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::KeepLastGood);
    CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::KeepLastGood);
    CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::Degrade);
}

TEST_CASE("DecideOnRead: with no picture to keep, a bad read just applies") {
    // Nothing was shielded, so there is nothing to uncover, and history
    // that never mattered must not count against a later good read.
    ReadPolicyState state;
    state.consecutiveBadReads = 5;
    CHECK(DecideOnRead(state, false, 0, /*haveLastGood=*/false) == ReadDecision::Apply);
    CHECK(state.consecutiveBadReads == 0);
}

TEST_CASE("DecideOnRead: keeps degrading once degraded, until a good read") {
    ReadPolicyState state;
    for (int i = 0; i < kBadReadsBeforeDegrade; ++i) {
        DecideOnRead(state, false, 0, true);
    }
    // Still bad and still nominally "having" a picture: stay degraded
    // rather than flipping back to holding a stale shield.
    CHECK(DecideOnRead(state, false, 0, true) == ReadDecision::Degrade);
}

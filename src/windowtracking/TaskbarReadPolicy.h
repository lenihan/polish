#pragma once

#include <cstddef>

namespace polish {

// What to do with the shield when a taskbar read comes back.
//
// Exists because the shield has an unusual failure economics: covering the
// strip too long costs nothing visible, while leaving it uncovered for even
// a fraction of a second can cost everything. The native flyout's dwell is
// 250-450 ms, and once the flyout is on screen nothing Polish can do covers
// it again (docs/LIMITATIONS.md #22). So a single bad read must never be
// allowed to uncover the strip.
//
// The previous behaviour did exactly that. A failed read -- and explorer
// restarting is precisely when reads fail -- hid the shield and cleared
// every target, and the next attempt was the 3 second safety-net timer:
// roughly ten flyout dwells of exposure, in the window where a user who has
// just restarted explorer is most likely to be hovering the taskbar.
enum class ReadDecision {
    // A good read: use it.
    Apply,
    // A bad read while there is something to keep: leave the shield and the
    // targets exactly as they were, and try again soon.
    KeepLastGood,
    // Enough consecutive bad reads that the last-known-good picture is no
    // longer believable: uncover the strip and stand down.
    Degrade,
};

struct ReadPolicyState {
    int consecutiveBadReads = 0;
};

// Consecutive bad reads tolerated before degrading. Three, at the retry
// cadence below, is roughly three quarters of a second of holding a stale
// shield -- long enough to ride out explorer starting up, short enough that
// a taskbar which really has gone does not stay shielded for long. A stale
// shield over a gone taskbar is harmless; there is nothing under it.
inline constexpr int kBadReadsBeforeDegrade = 3;

// How soon to try again after a bad read, in milliseconds. Comfortably under
// the flyout's 250 ms lower dwell bound would be ideal, but a UIA read itself
// takes ~15-60 ms and the goal is not to race the dwell -- it is that the
// shield is *still in place* throughout, which KeepLastGood guarantees. This
// only bounds how long a stale picture is held.
inline constexpr unsigned kBadReadRetryMs = 250;

// Decides what a read means and updates the running count.
//
//   usable          the taskbar could be read at all
//   buttonCount     how many app buttons the read returned
//   haveLastGood    whether there is a previous good picture to keep
//
// A read that is usable but empty is treated as bad while there is a last
// good picture. That is deliberate: UI Automation can return a tree with no
// task buttons in it mid-relayout, and "the taskbar genuinely has no apps"
// is indistinguishable from that on a single sample. The cost of being wrong
// about a genuinely empty taskbar is a few hundred milliseconds of shielding
// nothing.
ReadDecision DecideOnRead(ReadPolicyState& state, bool usable, std::size_t buttonCount, bool haveLastGood);

}  // namespace polish

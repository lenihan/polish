#pragma once

#include <windows.h>

#include <span>

namespace polish {

// The geometry behind sticky edges (see hook/StickyEdgeHook.h for the input
// side). Pure, like MoveSnap.h: no HWNDs, no monitor queries, no cursor calls,
// so it links into the test binary and is checked by
// tests/StickyEdgesTests.cpp.
//
// A barrier is a rect the pointer is held inside: a move that would take it
// from inside to outside is clamped to the edge instead, until the pointer has
// been pushed on past the edge by `threshold` pixels in total. Coming in is
// always free, and so is sliding along an edge -- only the axis that crosses is
// clamped. The top of a screen already behaves this way; barriers give the same
// "infinite edge" to an app's sides, the taskbar and the seam between monitors.

struct Barrier {
    RECT rect{};
};

// The push in progress. Default-constructed means none.
struct StickyState {
    int barrier = -1;  // index into the barrier list being pushed against
    int pushed = 0;    // pixels pushed past the edge so far
    ULONGLONG lastPushTick = 0;
};

struct StickyResult {
    bool hold = false;  // true: put the pointer at `clamped` instead of `to`
    POINT clamped{};
};

// A push that pauses this long starts again from nothing, so resting against an
// edge and later nudging it does not suddenly break through.
inline constexpr ULONGLONG kStickyPushResetMs = 500;
// Moving back in from the edge by more than this also starts the push again.
inline constexpr int kStickyBackOffPx = 4;
// Barriers whose edges are this close count as one edge: crossing it takes one
// push, not one per barrier. Covers an app's invisible resize border lying just
// past the work area it is snapped to.
inline constexpr int kStickySameEdgePx = 8;

// One pointer move from `from` to `to`, against `barriers` in order (innermost
// first). `threshold` is in the same pixels as the points.
StickyResult StepSticky(std::span<const Barrier> barriers, POINT from, POINT to, int threshold, StickyState& state,
                        ULONGLONG now);

// Sticky resize handles: when the pointer turns into a resize arrow (a
// splitter such as VS Code's side panel, or a window's border), it is held
// there for a moment so it is easy to grab. Which arrow says which way the
// handle drags, and only that axis is held -- along a vertical splitter the
// pointer still slides freely.
enum class ResizeAxis {
    None,
    Horizontal,  // <-> : x is held
    Vertical,    // up/down : y is held
    Both,        // a diagonal corner arrow
};

struct ResizeHoldState {
    bool active = false;  // holding at `anchor` now
    bool spent = false;   // this handle already let go; not again until the arrow goes away
    POINT anchor{};
    ULONGLONG since = 0;
    int pushed = 0;
};

// How long a resize handle holds the pointer, at most.
inline constexpr ULONGLONG kResizeHoldMs = 1000;

// One pointer move. `axis` is the arrow showing now (None for any other
// cursor). Lets go after kResizeHoldMs or once pushed `threshold` pixels off
// the handle, whichever comes first, and then stays let go until the arrow
// changes back.
StickyResult StepResizeHold(ResizeAxis axis, POINT from, POINT to, int threshold, ResizeHoldState& state,
                            ULONGLONG now);

}  // namespace polish

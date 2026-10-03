#pragma once

#include <windows.h>

#include <span>
#include <vector>

namespace polish {

// The geometry behind easy move/resize mode (see hook/MoveModeHook.h for
// the input side). Deliberately Win32-free past the RECT/POINT types: no
// HWNDs, no monitor queries, no SetWindowPos. Everything here is a pure
// function of rects, so it links into the test binary and the fiddly
// parts -- which edge wins a snap, what happens at exactly the threshold,
// what an oversized window clamps to -- are checked by
// tests/MoveSnapTests.cpp rather than by dragging windows around by hand.
// Same split as halo_math, bullseye_math and ComputeGridShape.
//
// COORDINATE SPACE: everything in this file is in *visible* rect space --
// GetVisibleWindowRect (windowtracking/RectUtils.h), not GetWindowRect.
// Snapping on raw GetWindowRect coordinates looks wrong rather than
// slightly off: the modern invisible resize border is ~7px per side, so
// two windows whose raw rects are flush show a ~14px gap between the
// edges the user can actually see. The caller converts back by sampling
// the per-edge inset once at grab time -- see the note on that in
// main.cpp, which explains why that sampling is safe here even though
// RectUtils.h warns against inset conversion for restore-position sync.

// Which part of a window a drag grabbed, under the zone model: a band
// around the window's edge resizes, and everything inside it moves.
//
// This replaced a two-button scheme (left button moved from anywhere,
// right button resized by the nearest quadrant). The right button was the
// problem: it is genuinely awkward on a touchpad, which is where this
// feature gets used most. One button and a hit-test is the trade -- you
// now have to be in the band to resize, but the band is an inch wide, so
// there is still nothing to aim at in the sense that matters.
//
// Eight resize zones rather than the old four, because an inch-wide band
// has room for the edges as well as the corners, and dragging one edge is
// what you usually want.
enum class Grip {
    // The whole rect translates; width and height never change.
    Move,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// Which edges a grip drags. Grip::Move answers false to all four: it
// translates the whole rect rather than moving any edge independently,
// and every function here special-cases it before asking.
bool GripMovesLeft(Grip grip);
bool GripMovesRight(Grip grip);
bool GripMovesTop(Grip grip);
bool GripMovesBottom(Grip grip);

// Whether a grip resizes at all, i.e. anything but Grip::Move.
bool GripResizes(Grip grip);

// Width of the resize band, in physical pixels, for a window of this
// size, given the band width actually wanted (one inch at the target's
// DPI -- which is just the DPI, since DPI is dots per inch).
//
// Clamped to 30% of each dimension so the band can never eat the window:
// a full inch on each side of a 1.5-inch-wide window would leave no
// middle to move by, and on a narrow window the two bands would overlap
// and the hit-test would have to break the tie arbitrarily. On a window
// too small for any sensible band this returns 0, which makes the whole
// window a move target -- the safe way round, since a window can always
// be resized from the keyboard but an unmovable one is stuck.
int ResizeBorderPx(const RECT& visible, int requestedPx);

// Which zone of `visible` holds `pt`, given a band of `borderPx`.
//
// Boundaries belong to the band: a point exactly `borderPx` in from the
// left edge is already Move, and the band covers [left, left+borderPx).
// A borderPx of 0 makes everything Move. A degenerate or empty rect
// likewise comes back Move, which does nothing rather than resizing
// something to nonsense.
Grip GripForPoint(const RECT& visible, POINT pt, int borderPx);

// The rect a zone occupies inside `visible`, given a band of `borderPx`.
//
// The inverse of GripForPoint, and deliberately next to it: the zone map
// the UI draws and the hit-test that decides what a click does are the
// same geometry, and they must not be able to drift apart -- a map that
// shows a band one pixel wider than the band that actually responds is a
// bug nobody would think to look for.
//
// Grip::Move returns the inner rectangle. A borderPx of 0 makes every
// zone the whole rect, matching GripForPoint answering Move everywhere.
RECT ZoneRect(Grip grip, const RECT& visible, int borderPx);

// A whole-monitor layout a move drag can be dropped into, the way
// dragging a window at a screen edge works natively.
//
// Separate from the magnetic edge snapping above, and triggered
// differently: that one attracts the *window* to nearby edges as a
// continuous nudge, this one is a discrete commitment driven by where
// the *pointer* is. They coexist because they answer different wants --
// "line this up with its neighbour" versus "give this half the screen".
enum class SnapLayout {
    None,
    Maximize,
    LeftHalf,
    RightHalf,
    TopLeftQuarter,
    TopRightQuarter,
    BottomLeftQuarter,
    BottomRightQuarter,
};

// Which layout the pointer is asking for, or None.
//
// `monitor` is the full monitor rect, not the work area: the trigger is
// the physical screen edge the pointer can actually run into, and the
// taskbar does not move it. Corners win over edges, so the corner zones
// are checked first -- otherwise a pointer in the top-left corner would
// maximize, and the quarter would be unreachable.
//
// The bottom edge deliberately means nothing. Natively it does not snap
// either, and claiming it would make dragging a window near the taskbar
// unpredictable.
SnapLayout LayoutForPointer(POINT pt, const RECT& monitor, int edgePx, int cornerPx);

// The rect a layout resolves to within `workArea` -- the work area, not
// the monitor, so a snapped window sits above the taskbar rather than
// under it. Halves and quarters tile exactly: the right half starts
// where the left half ends, with no overlap or gap at an odd width.
//
// SnapLayout::Maximize returns the whole work area, but a caller that can
// should prefer ShowWindow(SW_MAXIMIZE) so the window is genuinely
// maximized -- its own Restore button then works, and restore-position
// sync already knows what to do with it.
RECT RectForLayout(SnapLayout layout, const RECT& workArea);

// How close to a monitor edge the pointer has to get, in DIPs, before a
// layout is offered, and the size of the square at each corner that
// offers a quarter instead. The corner zone is much larger because a
// corner is harder to arrive at precisely, and because overshooting into
// a corner you did not want is a cheap mistake -- the preview shows what
// will happen before the button comes up.
inline constexpr int kLayoutEdgeDip = 6;
inline constexpr int kLayoutCornerDip = 40;

// Edges a dragged window can snap to, split by axis. Plain sorted,
// deduplicated coordinate lists rather than anything richer, because
// both kinds of snap the user wants fall out of the same set: testing the
// dragged rect's *left* against a neighbour's *right* gives edge-to-edge
// butting, and testing its left against that neighbour's left gives
// alignment. One list of x coordinates covers both.
struct SnapCandidates {
    // LONG, matching RECT's own members, so coordinates never round-trip
    // through a narrower type -- /W4 would flag it and there is nothing
    // to gain from it.
    std::vector<LONG> xEdges;
    std::vector<LONG> yEdges;
};

// `start` with the grip's own edges displaced by (dx, dy): the whole
// rect for Grip::Move, and only the edges that grip drags for a resize,
// leaving the others anchored. The one place the pointer
// delta turns into a rect, shared by the mouse drag and the keyboard
// session so the two cannot disagree about what a resize means.
RECT DragGrip(const RECT& start, Grip grip, int dx, int dy);

// Snap targets from the monitor's work area plus the visible rects of the
// other windows on that monitor.
//
// `workArea` is rcWork, not rcMonitor: a window should snap flush to the
// top of the usable desktop rather than underneath the taskbar. The hard
// containment limit is a separate thing and uses rcMonitor -- see
// ClampToMonitor.
//
// Filtering `others` to the relevant monitor, and to windows worth
// aligning to, is the caller's job; this takes whatever it is given.
SnapCandidates CollectSnapEdges(const RECT& workArea, std::span<const RECT> others);

// `desired` pulled onto the nearest candidate edge within `thresholdPx`,
// independently per axis.
//
// The snap is magnetic, never blocking: it is recomputed every frame from
// the raw cursor-derived rect, so it attracts while the user is within
// the threshold and simply stops attracting past it. Nothing accumulates
// and nothing has to be overridden with a modifier -- pushing a little
// further slides the window past its neighbour and lets windows overlap
// freely, which is the behavior asked for.
//
// For Grip::Move the whole rect translates, so both edges on an axis are
// offered to the candidate set and the winning delta shifts both. For a
// resize grip only the edges that grip drags are attracted; the anchored
// ones must not move.
RECT ApplySnap(const RECT& desired, const SnapCandidates& candidates, int thresholdPx, Grip grip);

// `desired` confined to `monitor` (rcMonitor, not rcWork).
//
// This is the hard limit, as opposed to ApplySnap's magnetism: it is what
// makes a window slammed at a shared monitor edge park flush against it
// instead of spilling onto the next screen, so the edge can be hit
// without aiming. Crossing to the next monitor is still possible by
// pushing well past the clamp -- see ShouldReleaseClamp.
//
// A window larger than the monitor on an axis cannot satisfy both edges.
// The near edge (left/top) wins, so the window stays reachable rather
// than having its title side pushed off-screen.
RECT ClampToMonitor(const RECT& desired, const RECT& monitor, Grip grip);

// How far past the clamp the pointer must push before the window may be
// let through to an adjacent monitor. Large enough that parking a window
// against a shared edge -- which means pushing *into* that edge and
// overshooting a little -- doesn't fling it onto the next screen, small
// enough to be discovered by just pushing harder.
inline constexpr int kOvershootReleasePx = 40;

// Whether the pointer has pushed far enough past the clamp to earn a
// release. `pushedPastClampPx` is the current distance between where the
// pointer asked the window to be and where the clamp put it -- a live
// measure, not a running total, so easing off re-engages the clamp
// instead of leaving a window primed to escape. Strictly greater than
// the threshold, so the boundary case holds the clamp.
//
// Necessary but NOT sufficient, and the caller must apply the other half:
// there has to be somewhere for the window to go. This test knows only
// about distance, so on its own it releases a window off the edge of the
// desktop as readily as onto a second screen -- confirmed by a probe that
// pushed 150px past the left edge of the only monitor and watched the
// window leave the screen. The caller's own check is that the *pointer*
// has reached a different monitor, which is impossible when there is no
// adjacent monitor in that direction. See main.cpp's OnMoveModeDrag.
bool ShouldReleaseClamp(int pushedPastClampPx);

// `current` translated so its leading edge lands flush on the next
// candidate edge in the given direction -- the keyboard equivalent of
// slamming a window somewhere, bound to Ctrl+Arrow.
//
// `dx`/`dy` are -1, 0 or +1 and are applied per axis. "Leading" means the
// edge facing the direction of travel: moving right, the right edge lands
// on the nearest candidate strictly to the right of it. Size is
// preserved. With no candidate ahead on an axis, that axis doesn't move.
RECT NextSnapInDirection(const RECT& current, const SnapCandidates& candidates, int dx, int dy);

// `desired` with its width/height floored, by pushing back whichever
// edges the grip is moving (the anchored edges stay put). Grip::Move is
// returned unchanged -- it cannot change size -- and so is an axis the
// grip does not drag, so a Top drag never adjusts the width.
RECT EnforceMinimumSize(const RECT& desired, Grip grip, int minWidth, int minHeight);

// How close an edge must come to a candidate to be pulled in, in physical
// pixels. 12 is a bit more than a window's invisible border, so the snap
// engages about when the visible edges look adjacent. User-overridable
// via Settings::moveModeSnapThresholdPx.
inline constexpr int kDefaultSnapThresholdPx = 12;

// How far one arrow key nudges a window in a keyboard session. Coarse
// enough to cross a screen in a reasonable number of presses; snapping
// still applies on top, and Ctrl+Arrow covers long distances in one jump.
inline constexpr int kKeyboardStepPx = 20;

// Floor for a resize, in physical pixels. Picked to leave a window big
// enough to still be grabbable by this very feature after a resize --
// a window shrunk to nothing has no move zone left in the middle. Apps
// with a larger minimum of their own enforce it themselves via
// WM_GETMINMAXINFO; this only stops Polish asking for something absurd.
inline constexpr int kMinWindowWidthPx = 160;
inline constexpr int kMinWindowHeightPx = 100;

// The resize band's width, in inches. An inch is a deliberately large
// target: this feature exists because the native ~7px border cannot be
// hit reliably, and a band that needs aiming at would reintroduce the
// problem it was built to solve. See ResizeBorderPx for how it shrinks
// on a small window.
inline constexpr double kResizeBorderInches = 1.0;

}  // namespace polish

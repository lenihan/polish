#pragma once

#include <windows.h>

#include <vector>

namespace polish {

// Where a handful of windows go when the desktop is arranged at once --
// as opposed to windowtracking/MoveSnap.h, which is about one window being
// dragged. Pure rect math, Win32-free past the RECT type, so the awkward
// parts (a work area whose width does not divide by three, a monitor
// turned on its side) are settled by tests rather than by opening windows
// and looking.
//
// There is no general "tile N windows" here, and that is the point. An
// earlier version arranged *every* normal window into a ceil(sqrt(n))
// grid, which answers a question nobody asks: past about four slots every
// window is too small to work in, so the result gets looked at once and
// undone. What people actually want is "I am working with two windows and
// want to see both", so the commands are named for their counts and each
// one takes exactly that many windows. Cascade is gone for the same
// reason -- a stack of overlapping windows is what Stacks does properly.

enum class ArrangeKind {
    // Two windows, each half the work area.
    TwoWay,
    // Three windows, each a third.
    ThreeWay,
    // Four windows, one per quarter.
    FourWay,
};

// How many windows this kind places. Lives beside the enum because it is
// the enum's own arity, not a policy -- ArrangeSelection.h holds the
// policy built on top of it.
int SlotsFor(ArrangeKind kind);

enum class Orientation { Landscape, Portrait };

// Which way round the work area is. A square work area is Landscape, by
// the >=; arbitrary, but it has to be one of them and a test pins it so
// it cannot quietly change.
Orientation OrientationOf(const RECT& work);

// `work` cut into `n` equal parts across its longer dimension: columns on
// a landscape work area, rows on a portrait one. Index 0 is the leftmost
// or topmost.
//
// Splitting the long way is the whole of "use the space the monitor
// actually has". Two windows side by side on a 16:9 monitor get a usable
// shape each; stacked, they get two letterboxes. Turn the monitor on its
// side and the same reasoning flips the split with it.
//
// The slices tile `work` exactly. Each boundary is computed once as
// left + width * i / n and used as both the previous slice's far edge and
// the next one's near edge, so no rounding can leave a seam, and the last
// slice lands exactly on the far edge however badly the division goes.
std::vector<RECT> SliceAlongLongAxis(const RECT& work, int n);

// One rect per window, in placement order, for `count` windows arranged
// `kind`-wise in `work`.
//
// Returns empty unless count == SlotsFor(kind). The caller decides
// whether a command is available at all (see ArrangeSelection.h) and this
// does not try to be forgiving about it: silently arranging three windows
// four ways would put a window in a slot nothing is tracking.
//
// Index 0 is the slot the first window in the caller's order gets, which
// is the most recently used one. That is what makes reversing the order a
// meaningful thing to ask for.
std::vector<RECT> ArrangeRects(const RECT& work, ArrangeKind kind, int count);

}  // namespace polish

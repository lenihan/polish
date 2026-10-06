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

// How wide each of `n` slots is along an axis of length `total`, when
// slot i cannot be narrower than minimums[i] (pass an empty vector for no
// minimums). The result sums to exactly `total`, in slot order.
//
// A slot that cannot shrink to an equal share is held at its minimum and
// the others divide what is left -- repeated, because holding one at its
// minimum shrinks the others' share and can push another over its own
// limit. So three windows in 2880px where the middle one needs 1500px
// give 1500 to it and 690 to each of the others, not three 960s with one
// of them quietly refusing and overlapping its neighbour.
//
// If the minimums cannot all be honoured at once (they add up to more than
// `total`) every slot is scaled down in proportion, with a floor on each
// weight so a window with no minimum is not squeezed to nothing. That is
// meant for a small shortfall -- CheckArrangeFit refuses anything bigger --
// where each stubborn window then overruns its slot by a few pixels.
std::vector<int> DistributeWithMinimums(int total, int n, const std::vector<int>& minimums);

// As ArrangeRects, but each window has a minimum size and the others
// share the space left around the ones that cannot shrink to their slot.
// minSizes is parallel to the windows (one per slot, in slot order) and in
// the same coordinate space as `work`. An empty vector means no minimums,
// which is exactly ArrangeRects.
//
// For 2-way and 3-way only the minimum along the split axis matters. For
// 4-way each column is held at least as wide as the widest minimum in it,
// and each row as tall as the tallest, so the slots still tile exactly.
std::vector<RECT> ArrangeRectsWithMinimums(const RECT& work, ArrangeKind kind, int count,
                                           const std::vector<SIZE>& minSizes);

// Whether `count` windows with these minimum sizes can be arranged
// `kind`-wise in `work` without any of them overlapping, and if not, which
// window is the one in the way.
//
// "Fits" means every window can be given a slot at least as big as its own
// minimum, with the slots still tiling `work`. When it does not, the
// arrangement is refused rather than made anyway: a tiling command that
// leaves windows sitting on top of each other has not done what it is
// named for, and the user is better told why than handed a mess.
struct ArrangeFit {
    bool fits = true;
    // Slot index of the window that is the problem -- the one with the
    // largest minimum along the axis that does not fit. -1 when fits.
    int blocker = -1;
    // Which dimension failed: width (true) or height (false).
    bool tooWide = true;
    // What the failing axis needs in total, what it has, and the blocking
    // window's own minimum along it. All in pixels, for the message.
    int needed = 0;
    int available = 0;
    int blockerMin = 0;
};

// minSizes is parallel to the windows, in slot order. Empty means no
// minimums, which always fits.
ArrangeFit CheckArrangeFit(const RECT& work, ArrangeKind kind, int count, const std::vector<SIZE>& minSizes);

// As CheckArrangeFit, but tries every assignment of the windows to slots
// and succeeds if any one fits. In 4-way it matters which windows share a
// column or row: Outlook needing 1286px of height fits beside a window
// needing 757px (a 1286px row and a 538px row), but not above two windows
// whose tallest needs 757px (1286 + 757 > 1824). So "does not fit" has to
// mean "no arrangement fits", not "this order does not fit". For 2-way and
// 3-way the order is irrelevant and this is the same as CheckArrangeFit.
//
// On failure the report is for the order as given, so the blocker index
// refers to the caller's own ordering.
//
// An earlier version tried only rotations of the order, which is what the
// repeat-to-rotate toggle produces. That was wrong: rotation keeps
// opposite windows opposite, so two particular windows could never be put
// in the same row, and a layout that existed was reported as impossible.
ArrangeFit CheckArrangeFitAnyOrder(const RECT& work, ArrangeKind kind, const std::vector<SIZE>& minSizes);

}  // namespace polish

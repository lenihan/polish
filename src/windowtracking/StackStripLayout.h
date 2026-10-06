#pragma once

#include <windows.h>

#include <optional>
#include <vector>

#include "windowtracking/StackState.h"

namespace polish {

// The geometry of a stack's tab strip: where each tab goes, which tab a
// point is over, where a dragged tab should land, and when a drag has left
// the strip altogether. Pure rect math, Win32-free past RECT/POINT, so the
// arithmetic that is easiest to get off by one -- reordering, hit-testing at
// tab edges -- is pinned by tests instead of by dragging tabs around.
//
// All values are device pixels; the caller scales its DIP metrics first.

struct TabMetrics {
    // Along the strip. A tab is as wide as an equal share of the room, but
    // never wider than maxWidth. Past the point where equal shares would
    // drop below floorWidth, tabs stop shrinking and the ones that do not
    // fit are left off rather than drawn outside the strip -- a tab
    // rendered where the strip is not is a click that goes nowhere.
    int maxWidth = 220;
    int floorWidth = 44;
    // Space between neighbouring tabs, and before the first / after the
    // last along the strip.
    int gap = 4;
    int leadingPad = 6;
    // Across the strip: space above and below each tab (Horizontal) or
    // left and right of it (Vertical).
    int crossPad = 4;
    // Vertical strips only: every tab is this tall, and they stack
    // downward. (In a Horizontal strip the tab is as tall as the strip
    // less crossPad.)
    int rowHeight = 32;
};

// One rect per tab that fits, in order, inside `strip`. May return fewer
// than `tabCount` when they cannot all fit even at floorWidth.
std::vector<RECT> TabRects(const RECT& strip, int tabCount, StackAlignment alignment, const TabMetrics& metrics);

// Index of the tab containing `pt`, or nullopt for a gap, the margin, or
// anywhere outside every tab. Rects are half-open like a RECT's own
// PtInRect: the left/top edge is in, the right/bottom edge is not.
std::optional<size_t> TabIndexAt(const std::vector<RECT>& rects, POINT pt);

// Where the tab being dragged (`dragged`) should be moved to while the
// pointer is at `pt`. It follows the pointer across tabs; past either end
// of the row it goes to the first or last slot; over a gap it stays where
// it is, so a pointer wobbling between two tabs does not shuffle them.
size_t ReorderTargetIndex(const std::vector<RECT>& rects, size_t dragged, POINT pt, StackAlignment alignment);

// Whether a tab drag at `pt` has left the strip far enough to count as
// taking the window out of the stack rather than reordering it.
//
// One function with one threshold on purpose. Reordering and tearing out
// are the same gesture told apart only by where it ends, and if two pieces
// of code each decided it, they would disagree at the boundary.
bool DragTearsOut(const RECT& strip, POINT pt, int thresholdPx);

// Which slot a window dropped on the strip at `pt` would be inserted at:
// before the tab the point is on the leading half of, after one it is on the
// trailing half of. Index == rects.size() means "at the end". Used for the
// insertion caret while a window is dragged over a strip to join the stack.
size_t InsertionIndexAt(const std::vector<RECT>& rects, POINT pt, StackAlignment alignment);

}  // namespace polish

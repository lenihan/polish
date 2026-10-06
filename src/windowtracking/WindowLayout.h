#pragma once

#include <windows.h>

#include <vector>

namespace polish {

// Where a set of windows goes when the whole desktop is arranged at once
// -- as opposed to windowtracking/MoveSnap.h, which is about one window
// being dragged. Pure rect math, Win32-free past the RECT type, so the
// awkward parts (an odd count that does not fill its grid, a cascade
// that runs out of room) are settled by tests rather than by opening
// eleven windows and looking.
//
// Deliberately not built on GroupManager's ComputeGridShape, which
// computes the same kind of shape for a group's tiles. That one takes
// GroupMode/GroupAlignment and belongs to a subsystem PLAN.md has marked
// for removal ("Remove Groups: Too hacky"), and the arithmetic it would
// save is one ceil(sqrt). Borrowing it would tie a new feature to code
// that is scheduled to be deleted.

enum class ArrangeKind {
    // Every window gets its own slot in a roughly square grid.
    Tile,
    // Every window gets the same size, stepped down and right from the
    // work area's top-left so each one's title bar stays visible.
    Cascade,
};

struct LayoutGrid {
    int cols;
    int rows;
};

// The grid for `count` windows: ceil(sqrt(count)) columns, and however
// many rows that needs. Biased wide because screens are, so three
// windows become two over one rather than one over two.
//
// Two useful cases fall out rather than being special-cased: a count of
// 2 is a left/right split, and a count of 4 is the four corners.
LayoutGrid TileShape(int count);

// One rect per window, row-major, tiling `work` exactly -- adjacent
// slots share an edge, so there is no gap or overlap however the
// division rounds.
//
// A final row that is short is stretched to the full width rather than
// left with a hole: three windows read better as two over one full-width
// than as two over one with a gap beside it.
std::vector<RECT> TileRects(const RECT& work, int count);

// One rect per window, each the same size, stepped `stepPx` down and
// right. Index 0 is the top-left of the stack.
//
// The step restarts from the top-left once another one would not fit,
// which is what Windows' own cascade does: a long enough run otherwise
// walks the last windows off the bottom of the screen entirely.
std::vector<RECT> CascadeRects(const RECT& work, int count, int stepPx);

// How far each cascaded window is offset from the one before, in DIPs --
// enough to leave a title bar and a window edge visible, which is the
// only thing a cascade is for.
inline constexpr int kCascadeStepDip = 30;

// A cascaded window's size as a fraction of the work area. Big enough to
// be worth looking at, small enough that several steps fit before the
// stack has to restart.
inline constexpr int kCascadeWidthPercent = 60;
inline constexpr int kCascadeHeightPercent = 70;

}  // namespace polish

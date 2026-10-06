#pragma once

#include <windows.h>

#include "windowtracking/StackState.h"

namespace polish {

// Where a stack's tab strip and its members go, given the one rectangle
// that defines the stack. Pure rect math, Win32-free past RECT, so the
// invariant everything else leans on -- that going from a stack rect to its
// content and back is exact -- is settled by tests.
//
// A stack is one rect. Every member is positioned into the *content* part
// of it, and the tab strip sits in the rest: a band along the top
// (Horizontal) or a column down the left (Vertical). The strip has no
// position of its own; it is recomputed from the stack rect on every
// reflow, which is what makes "the strip drifted away from its stack"
// impossible rather than something to defend against.

struct StackFrame {
    RECT strip;
    RECT content;
};

// Splits `stackRect` into the strip and the content. The strip is
// `stripThicknessPx` tall (Horizontal, spanning the full width) or wide
// (Vertical, spanning the full height); the content is the remainder.
//
// Degenerate sizes are clamped rather than inverted: a stack rect smaller
// than the strip gives a strip that fills it and content one pixel in the
// direction it would have been, never a negative rect. Callers keep stacks
// above a sensible minimum; this just refuses to produce nonsense.
StackFrame ComputeStackFrame(const RECT& stackRect, StackAlignment alignment, int stripThicknessPx);

// The inverse: the stack rect whose content part is `content`. Exact --
// ComputeStackFrame(StackRectFromContent(c, a, t), a, t).content == c --
// which matters because adopting a member's rect (it was resized by hand)
// goes content -> stack rect -> content again, and a one-pixel error there
// would nudge every member by a pixel on every resize.
RECT StackRectFromContent(const RECT& content, StackAlignment alignment, int stripThicknessPx);

}  // namespace polish

#pragma once

#include <windows.h>

namespace polish {

// Whether two rects are equal within `epsilonPixels` on each edge, to
// tolerate Windows silently clamping a requested rect (e.g. to the work
// area) rather than honoring it exactly.
bool RectsApproximatelyEqual(const RECT& a, const RECT& b, int epsilonPixels = 2);

// A window's *visually* occupied screen rect, for code that needs to
// draw something aligned to what the user actually sees.
// GetWindowRect includes the modern invisible resize border (present even
// on a window with no visible frame there), which Windows deliberately
// hangs a few px *off* the monitor's edges for a maximized window so the
// window's actually-visible edge lines up exactly with the screen edge.
// Drawing a thin ring on the raw GetWindowRect rect of a maximized window
// therefore lands most or all of it off-screen -- confirmed as a real,
// human-reported bug (invisible ring on a maximized/full-screen app).
// DWMWA_EXTENDED_FRAME_BOUNDS gives the tighter, actually-visible rect
// instead (excludes that invisible border), falling back to
// GetWindowRect if DWM can't answer.
//
// This is for a single fresh query feeding a one-off visual, never a
// stored/round-tripped value mixed with GetWindowRect from another code
// path -- see the restore-position-sync note below for why mixing the
// two across a round trip is the thing to avoid.
bool GetVisibleWindowRect(HWND hwnd, RECT& rect);

// Deliberately no DWM-extended-frame-bounds ("visual bounds") conversion
// in *restore-position sync*. An earlier version of this file had one
// (recording a window's visual bounds and converting back to
// GetWindowRect-style coordinates
// via a computed inset), on the theory that it would produce more
// visually accurate restores. In practice it caused a real bug: the
// invisible resize-border inset is not a fixed per-window constant --
// Windows appears to compute it differently for snapped-flush edges than
// for a free-floating window, so insets queried at *toggle time* did not
// match the insets in effect when the *target* rect was originally
// recorded, producing a restored window that was the right size but
// visually offset by a few pixels. Recording and restoring raw
// GetWindowRect() values directly (no conversion at all) sidesteps this
// entirely: whatever GetWindowRect reports for a given visual state is,
// by construction, exactly what SetWindowPos needs to reproduce that
// same state later -- no correction needed, since we're always replaying
// a rect this exact window already reported for itself.

}  // namespace polish

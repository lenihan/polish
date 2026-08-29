#pragma once

#include <windows.h>

namespace polish {

// Whether two rects are equal within `epsilonPixels` on each edge, to
// tolerate Windows silently clamping a requested rect (e.g. to the work
// area) rather than honoring it exactly.
bool RectsApproximatelyEqual(const RECT& a, const RECT& b, int epsilonPixels = 2);

// Deliberately no DWM-extended-frame-bounds ("visual bounds") conversion
// here. An earlier version of this file had one (recording a window's
// visual bounds and converting back to GetWindowRect-style coordinates
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

#pragma once

#include <windows.h>

#include <optional>

namespace polish {

// The rect math behind scaled windows: a window shrunk to a live thumbnail
// that goes back to 1:1 when clicked. Pure and Win32-free past RECT/POINT/
// SIZE, so the fiddly parts -- which corner stays put during a drag, where
// a restored window lands when its thumbnail was near an edge, how a
// scale snaps to a preset -- are settled by tests rather than by shrinking
// windows and looking.
//
// Everything is in physical pixels. A scale is a plain ratio, so moving a
// view between monitors with different DPI changes nothing here: the view
// is `scale` times the window's real size wherever it is.
//
// "Real size" throughout is the window's *visible* size (the DWM frame
// bounds), not its GetWindowRect size, which includes an invisible
// resize border and shadow. The view is of what the user sees.

// Bounds on a scale. Below 10% a window is a smudge; above 100% it would
// be a magnifier, which is not what this is.
constexpr double kMinScale = 0.10;
constexpr double kMaxScale = 1.0;

// The steps the panel and shortcuts move between, largest first. A drag
// snaps to these when it lands near one.
constexpr double kScalePresets[] = {0.5, 0.33, 0.25};
constexpr int kScalePresetCount = static_cast<int>(sizeof(kScalePresets) / sizeof(kScalePresets[0]));

enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };

// `real` scaled, rounded to the nearest pixel and never below 1x1 -- a
// zero-sized view cannot be hit or drawn.
SIZE ScaledSize(SIZE real, double scale);

// The view of a window of `real` size at `scale`, with its top-left at
// `topLeft`.
RECT ScaledViewRect(SIZE real, double scale, POINT topLeft);

// The scale a view of this rect represents for a window of `real` size.
// Taken from the width; aspect is locked, so height would say the same up
// to rounding.
double ScaleOf(const RECT& view, SIZE real);

// Where the real window goes when the view is clicked: `real` size with
// its top-left at the view's, shifted just enough to sit inside `work`.
// Anchoring at the thumbnail's position is the point -- the window grows
// out of where the user was looking.
//
// A window larger than the work area is pinned to its top-left corner and
// not shrunk: restoring must give back 1:1, and the user can move it.
RECT UnscaledRectAt(SIZE real, POINT viewTopLeft, const RECT& work);

// `view` moved just far enough to sit inside `work`, size untouched. For
// a display change or a monitor that went away. Larger than `work` pins
// to the top-left, like UnscaledRectAt.
RECT ClampViewToWorkArea(const RECT& view, const RECT& work);

// Dragging `grabbed` of a view: the opposite corner stays fixed and the
// grabbed one follows the pointer, with the aspect ratio of `real` locked.
//
// The scale comes from the pointer's position relative to the fixed
// corner, taking whichever axis asks for the larger view, rather than
// accumulating per-event deltas -- so rounding cannot build up into a
// ratio drift over a long drag, and the corner tracks the pointer exactly
// along the dominant axis. A pointer on the wrong side of the fixed
// corner clamps to `minScale`.
RECT CornerDragAspectLocked(const RECT& start, Corner grabbed, POINT pointer, SIZE real,
                            double minScale = kMinScale, double maxScale = kMaxScale);

// `scale` snapped to the nearest preset when within `tolerance` of it
// (absolute, so 0.03 means three percentage points), otherwise unchanged.
// 1.0 also snaps: dragging up to full size should land on full size.
double SnapScale(double scale, double tolerance = 0.03);

// One step along the presets. `direction` < 0 shrinks, > 0 grows.
// Returns the new scale, or nullopt for "grow past the largest preset":
// the caller restores the window to 1:1 rather than showing a view that
// is nearly, but not quite, the real thing. Shrinking below the smallest
// preset stays where it is.
std::optional<double> StepScale(double current, int direction);

// The view after the real window changed size on its own (the app
// resized itself while scaled): same top-left, same scale, new size.
RECT ResizeViewKeepingAnchor(const RECT& view, double scale, SIZE newReal);

}  // namespace polish

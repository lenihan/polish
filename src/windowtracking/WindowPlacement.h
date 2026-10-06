#pragma once

#include <windows.h>

namespace polish {

// Helpers for putting another process's window where you mean it to be, in
// the coordinate space people actually see.
//
// Windows have an invisible border (the resize frame) that GetWindowRect
// includes and the screen does not show -- about 7px each side, scaled by
// DPI, and different between apps and between a window's maximized and
// restored states. Two windows placed "flush" by raw GetWindowRect
// coordinates show a ~14px gap between their visible edges. Everything that
// positions windows here therefore works in *visible* rects
// (DWMWA_EXTENDED_FRAME_BOUNDS) and converts through a per-edge inset only
// at the moment it calls SetWindowPos.

// hwnd's visible rect, and the inset that converts back: windowRect =
// visible + inset, per edge. Sampled fresh each call and never stored --
// RectUtils.h explains why a stored inset goes wrong when a window changes
// state between sampling and use.
RECT VisibleRectAndInset(HWND hwnd, RECT& insetOut);

// The smallest *visible* size hwnd will accept, as it reports it through
// WM_GETMINMAXINFO -- the message an app answers to say "no smaller than
// this".
//
// Asked with a timeout and ABORTIFHUNG, because this is a cross-process
// SendMessage on the thread that also runs the low-level hooks: a stalled
// target must cost a bounded delay, not freeze them. A window that does not
// answer is treated as having no minimum.
//
// The window reports a window-rect size, converted here to visible space by
// removing the invisible border. A maximized window gets NO border removed:
// its invisible border is larger while maximized than once restored
// (measured: 26px vs 11px tall at 192 DPI), so removing it would understate
// the minimum the window will have in its slot, and an understated minimum
// is the harmful direction -- the slot comes out too small and the window
// overruns it. Leaving the border in overstates by about ten pixels, which
// only ever gives the window a little extra room.
SIZE MinimumVisibleSizeFor(HWND hwnd);

// Moves and sizes hwnd so its *visible* rect is `visible`. Restores it first
// if it is maximized (SetWindowPos silently does nothing to a maximized
// window's size and position), and does it asynchronously so a hung target
// cannot stall the caller. Z-order and activation are left alone.
//
// The restore is synchronous and happens before the inset is sampled, not
// after: a maximized window's border differs from a restored one's, so an
// inset taken first would leave the window a few pixels off.
void PlaceWindowVisible(HWND hwnd, const RECT& visible);

}  // namespace polish

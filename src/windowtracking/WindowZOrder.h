#pragma once

#include <windows.h>

namespace polish {

// Brings hwnd to the front of the normal (non-topmost) z-order band,
// without leaving it stuck topmost and without activating/focusing it
// (SWP_NOACTIVATE) -- promotes to HWND_TOPMOST, then *immediately*
// demotes back to HWND_NOTOPMOST. SWP_NOACTIVATE alone doesn't reliably
// force DWM to fully recompute z-order for a window that's never
// actually activated; the brief topmost pulse does. First proven by
// Alt+Tab's ApplyAltTabDimming (src/main.cpp) for bringing the
// highlighted candidate to front over other candidates/dim overlays;
// reused here (M4) for a group's active member. Returns false if the
// initial promote failed (most likely UIPI blocking a more-privileged
// target window -- see docs/LIMITATIONS.md #1), in which case the
// demote is skipped since there'd be nothing to undo.
bool PromoteWindowToFront(HWND hwnd);

}  // namespace polish

#pragma once

#include <windows.h>

#include <optional>

namespace polish {

// The window style captured before ReparentIntoGroup changes it --
// Windows has no query for "what was this window's style before I
// changed it," so the caller must hold onto this and pass it back to
// RestoreTopLevel later. (Only GWL_STYLE is touched -- GWL_EXSTYLE's
// bits that matter for top-level windows, like WS_EX_APPWINDOW/
// WS_EX_TOOLWINDOW, don't do anything meaningful on a child window, so
// there's nothing there worth changing or restoring.)
struct ReparentBackup {
    LONG_PTR style = 0;
};

// Turns hwnd into a real child of newParent, for true visual
// containment within a group (can't be dragged outside the parent's
// client area, unlike the reposition-only approach this replaced).
// Strips hwnd's own frame (title bar, resize border, minimize/maximize/
// system menu) since the group's own chrome now provides that context,
// adds WS_CHILD, and calls SetParent.
//
// Returns the pre-change style for later restoration, or std::nullopt if
// SetParent failed -- in which case hwnd is left exactly as it was found
// (the style change is rolled back) and the caller must not treat it as
// a member. That failure path is load-bearing, not defensive: an earlier
// version discarded SetParent's result entirely, so a failure left the
// window WS_CHILD with the *desktop* as its parent -- a state Windows
// simply does not render, producing a member that held a tab and a tile
// but drew nothing at all (confirmed, human-reported, with Calculator).
//
// Known, accepted tradeoff (deliberate, not overlooked): a WS_CHILD
// window is invisible to EnumWindows, so a reparented member no longer
// appears as its own entry in Alt+Tab, the taskbar, or the group
// picker's candidate list -- only the group's own chrome remains
// individually reachable. This is the whole point for the intended use
// case (collapsing many windows, e.g. 20 Notepads, into a handful of
// Alt+Tab-reachable groups), not a bug.
std::optional<ReparentBackup> ReparentIntoGroup(HWND hwnd, HWND newParent);

// Reverses ReparentIntoGroup: restores the original style/ex-style,
// reparents back to the desktop (nullptr), and forces Windows to
// recompute the non-client frame (SWP_FRAMECHANGED) so the title bar/
// borders reappear correctly. Must be called for every remaining member
// before a group's chrome window is destroyed -- a child window whose
// parent is destroyed while still parented is destroyed with it, so
// skipping this on cleanup would silently close (and lose unsaved work
// in) every window still in the group.
void RestoreTopLevel(HWND hwnd, const ReparentBackup& backup);

}  // namespace polish

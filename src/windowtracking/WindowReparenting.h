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

// The pre-change owner captured before AttachToGroup changes it -- same
// "Windows has no query for the previous value" reasoning as
// ReparentBackup.
struct AttachBackup {
    HWND owner = nullptr;
};

// The fallback for a window ReparentIntoGroup can never actually contain
// -- a UWP frame window (ApplicationFrameWindow), confirmed live:
// SetParent fails outright with ERROR_INVALID_PARAMETER for these every
// time, no DPI-hosting opt-in changes that. Rather than refusing such a
// window entirely, this keeps it top-level and makes `chrome` its owner
// (GWLP_HWNDPARENT) instead of its parent -- an owned window is kept
// above its owner in Z-order and is hidden/minimized/destroyed along
// with it by Windows itself, without needing WS_CHILD at all. Unlike
// ReparentIntoGroup, hwnd's frame styles are left completely alone: a
// second process (ApplicationFrameHost) draws that frame, not this app,
// and an attached member visibly keeping its own title bar is the
// honest cue that it's corralled, not contained -- see
// GroupManager::ApplyLayout's own comment on the "embedded vs. attached"
// split this enables.
//
// Whether GWLP_HWNDPARENT actually sticks cross-process against a real
// UWP frame is unverified as of this writing -- the caller should read
// GetWindow(hwnd, GW_OWNER) back and log it (see EnsureAttached). If it
// doesn't stick, the member still works as a position-synced member
// (GroupManager::EnforceMemberRect doesn't care how it's attached) but
// won't hide/restore with the group on its own.
//
// Always "succeeds" in the sense of returning a backup -- there's no
// SetParent-style failure mode here, GWLP_HWNDPARENT is just a style
// field -- but see the paragraph above for why "succeeds" doesn't
// necessarily mean "took effect."
AttachBackup AttachToGroup(HWND hwnd, HWND chrome);

// Reverses AttachToGroup: restores the original owner. Must be called
// for every attached member before a group's chrome window is destroyed
// -- same hazard RestoreTopLevel exists for on the embedded side: an
// *owned* window (not just a child) is also destroyed along with its
// owner, so skipping this on cleanup would silently close every
// attached member too.
void DetachFromGroup(HWND hwnd, const AttachBackup& backup);

}  // namespace polish

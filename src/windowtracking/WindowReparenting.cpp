#include "windowtracking/WindowReparenting.h"

namespace polish {

namespace {
// Frame-related style bits stripped on reparent -- the group's own
// chrome provides the title bar/tab strip context now, so a member
// showing its own title bar/resize border/system menu inside the
// content area would just be visual clutter, not a second way to
// control the window (GroupManager owns its position/size entirely
// once it's a member).
constexpr LONG_PTR kFrameStyleBits =
    WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_POPUP;
}  // namespace

ReparentBackup ReparentIntoGroup(HWND hwnd, HWND newParent) {
    ReparentBackup backup;
    backup.style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    const LONG_PTR newStyle = (backup.style & ~kFrameStyleBits) | WS_CHILD;
    SetWindowLongPtrW(hwnd, GWL_STYLE, newStyle);
    SetParent(hwnd, newParent);
    // SWP_FRAMECHANGED forces Windows to recompute the non-client area
    // from the new style -- a plain SetWindowLongPtr doesn't take
    // visual effect on its own until something triggers that
    // recalculation.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return backup;
}

void RestoreTopLevel(HWND hwnd, const ReparentBackup& backup) {
    SetWindowLongPtrW(hwnd, GWL_STYLE, backup.style);
    SetParent(hwnd, nullptr);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

}  // namespace polish

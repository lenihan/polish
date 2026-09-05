#pragma once

#include <windows.h>

namespace polish {

// Whether hwnd looks like a normal top-level application window worth
// treating as a switchable/groupable candidate at all, regardless of
// whether it's currently minimized -- mirrors the heuristic the
// taskbar/Alt-Tab use. Filters out tooltips, popups, IME windows, etc.
// IsCandidateWindow and IsMinimizedCandidateWindow both build on this,
// splitting only on IsIconic.
bool IsCandidateWindowShape(HWND hwnd);

// IsCandidateWindowShape(hwnd) && not minimized -- the normal Alt+Tab
// cycle / group-picker candidate check, unchanged from before this was
// split out. Shared by the Alt+Tab candidate list and the group window
// picker.
bool IsCandidateWindow(HWND hwnd);

// IsCandidateWindowShape(hwnd) && minimized -- Alt+Tab's separate
// minimized-windows section (see PLAN.md's Alt+Tab-improvements M4)
// reaches windows the normal cycle deliberately skips. IsWindowVisible
// is still correct to require here: a minimized window still carries
// WS_VISIBLE, so this and IsCandidateWindow can never both match the
// same window.
bool IsMinimizedCandidateWindow(HWND hwnd);

// Whether hwnd's owning process runs at a higher integrity level than
// this one (elevated, while Polish itself is not). SetWindowPos and
// friends are silently blocked by UIPI in that case (see
// docs/LIMITATIONS.md #1) -- Alt+Tab just logs and accepts the resulting
// no-op, but the group picker needs real, working control over every
// member it offers, so it excludes these entirely rather than letting
// someone add a window that then silently can't be repositioned.
bool IsElevatedWindow(HWND hwnd);

}  // namespace polish

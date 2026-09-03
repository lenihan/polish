#pragma once

#include <windows.h>

namespace polish {

// The user's actual chosen app theme (Settings > Personalization >
// Colors > "Choose your mode") -- same registry value every dark-mode-
// aware Win32 app reads; no public API for it.
bool IsDarkModeEnabled();

// Applies (or removes) the dark native title bar/frame on a top-level
// window (DWMWA_USE_IMMERSIVE_DARK_MODE) so the OS-drawn caption
// matches everything else a window paints itself.
void ApplyDarkTitleBar(HWND hwnd, bool dark);

// Opts a native context menu about to be shown by `ownerWindow` (via
// TrackPopupMenu/TrackPopupMenuEx) into the OS's live dark/light menu
// theme. There's still no documented public API for this as of this
// writing -- uses the same uxtheme.dll ordinal exports
// (SetPreferredAppMode/FlushMenuThemes) most dark-mode-aware Win32
// utilities rely on. Call this right before every TrackPopupMenu call,
// not just once at startup -- it re-reads the live system setting each
// time, so a theme change without restarting Polish still takes effect
// on the next menu shown.
void ApplyDarkModeToMenu(HWND ownerWindow);

}  // namespace polish

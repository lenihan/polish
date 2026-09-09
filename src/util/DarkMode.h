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

// Same idea as ApplyDarkModeToMenu, but for a tooltip common control
// (TOOLTIPS_CLASSW) -- comctl32's tooltip understands the same
// "DarkMode_Explorer" pseudo-theme directly (its background/text/border
// all follow from the theme class switch, no separate
// TTM_SETTIPBKCOLOR/TTM_SETTIPTEXTCOLOR needed -- those two are the
// *old*, pre-visual-styles way of coloring a tooltip and are actually
// ignored once a theme is applied). Call this fresh every time before
// showing the tooltip, not just once at creation, same reasoning as
// ApplyDarkModeToMenu's own comment about live theme changes.
void ApplyDarkModeToTooltip(HWND tooltipWindow);

// The user's chosen Windows accent color (Settings > Personalization >
// Colors), for highlighting something as interactive/draggable the
// same way the OS itself would. Reads
// HKCU\Software\Microsoft\Windows\DWM\AccentColor directly -- no public
// API for this either; that value's low 3 bytes are already in
// COLORREF's own 0x00BBGGRR layout, so no channel reordering is
// needed, just masking off its top (alpha) byte. Falls back to
// GetSysColor(COLOR_HIGHLIGHT) if the registry value is missing.
COLORREF GetAccentColor();

}  // namespace polish

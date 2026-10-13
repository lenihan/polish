#pragma once

#include <windows.h>

// objidl.h before gdiplus.h: GDI+ headers use IStream and friends without
// declaring them, and this project builds WIN32_LEAN_AND_MEAN.
#include <objidl.h>

#include <gdiplus.h>

namespace polish {

// What it takes to paint something that looks like it belongs on the
// Windows 11 taskbar. Shared by the shield (its hover highlight) and the
// Polish button, so the two cannot drift apart: a button that is a shade
// off from the highlight beside it is the kind of thing that reads as
// "not native" without anyone being able to say why.

// One-time GDI+ startup, as a function-local static: Polish is resident
// for its whole life and exits the process directly, so there is no
// meaningful moment to call GdiplusShutdown.
void EnsureGdiplusStarted();

// Whether the taskbar is drawn dark. The taskbar follows "Windows mode"
// (SystemUsesLightTheme), a different setting from the app theme every
// other window here asks IsDarkModeEnabled() about (AppsUseLightTheme);
// the two can be set independently, and this paint lands on the taskbar.
bool TaskbarIsDark();

// Premultiplies B/G/R by A in place. Required before UpdateLayeredWindow
// with ULW_ALPHA, and not something GDI+ does when drawing into a buffer
// we own.
void PremultiplyAlpha(BYTE* pixels, int pixelCount);

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius);

// The hover/pressed fill the taskbar draws behind a button: a faint
// rounded rectangle inset from the button's cell, light on a dark taskbar
// and dark on a light one. `cell` is in the bitmap's own coordinates and
// `dpi` scales the DIP constants.
//
// `strength` is 1.0 for hover and lower for the pressed state, which the
// native taskbar draws fainter still.
void DrawTaskbarFill(Gdiplus::Graphics& graphics, const Gdiplus::RectF& cell, UINT dpi, float strength);

}  // namespace polish

#pragma once

#include <windows.h>

namespace polish {

// A rounded-rectangle glow drawn directly on the currently-highlighted
// Alt+Tab candidate's own rect -- solid accent color at its outer edge,
// fading inward to fully transparent by the time it reaches the target's
// actual content (not projecting outward into the desktop margin around
// it) -- an active signal that this is the window you're about to switch
// to, rather than relying solely on "the one that isn't dimmed"
// (AltTabDimOverlay) to make that obvious.
//
// The fade needs real per-pixel alpha (a flat SetLayeredWindowAttributes
// alpha can't do a gradient), so this reuses this project's own
// established DIB + GDI+ + manual-premultiply + UpdateLayeredWindow
// rendering pipeline (see AltTabHighlightBorder.cpp) -- the same
// technique, and the same alpha gotcha it exists to work around
// (Gdiplus::Graphics(HDC) does not reliably preserve alpha), that
// PLAN.md's history already documents from earlier UI work in this app.
class AltTabHighlightBorder {
public:
    explicit AltTabHighlightBorder(HINSTANCE instance);
    ~AltTabHighlightBorder();

    AltTabHighlightBorder(const AltTabHighlightBorder&) = delete;
    AltTabHighlightBorder& operator=(const AltTabHighlightBorder&) = delete;

    // Positions the glow directly on target's current screen rect,
    // renders it, and makes it visible.
    void ShowAroundTarget(HWND target);

    // Hides the glow.
    void Hide();

private:
    HWND window_ = nullptr;
};

}  // namespace polish

#pragma once

#include <windows.h>

namespace polish {

// A thin, solid (fully opaque) rounded-rectangle ring drawn directly on
// the currently-highlighted Alt+Tab candidate's own rect, on its inner
// edge (not projecting outward into the desktop margin around it) -- an
// active signal that this is the window you're about to switch to, rather
// than relying solely on "the one that isn't dimmed" (AltTabDimOverlay) to
// make that obvious.
//
// A hollow ring still needs real per-pixel alpha (transparent everywhere
// except the ring itself -- a flat SetLayeredWindowAttributes alpha
// applies uniformly to the whole window, it can't carve out a shape), so
// this reuses this project's own established DIB + GDI+ +
// manual-premultiply + UpdateLayeredWindow rendering pipeline (see
// AltTabHighlightBorder.cpp) -- the same technique, and the same alpha
// gotcha it exists to work around (Gdiplus::Graphics(HDC) does not
// reliably preserve alpha), that PLAN.md's history already documents from
// earlier UI work in this app. (An earlier version of this class drew a
// soft gradient glow instead of a solid ring, which is why this pipeline
// was adopted in the first place -- kept even though the fill itself is
// now flat, since it's still what makes antialiased rounded corners on a
// hollow shape possible.)
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

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
    // `alwaysOnTop` (default true): the real Alt+Tab switcher overlay
    // needs to stay above literally every other window for the life of
    // the switcher session, matching Windows' own Alt+Tab -- this is
    // what WS_EX_TOPMOST is for. Pass false for any other reuse of this
    // class (e.g. a group's active-tile ring), where the highlight
    // should behave like an ordinary window: covered by whatever the
    // user brings to the front, not floating above unrelated apps
    // forever. See ShowAroundTarget's `zOrderAnchor` for how a
    // non-topmost instance stays visually attached to its owner instead.
    explicit AltTabHighlightBorder(HINSTANCE instance, bool alwaysOnTop = true);
    ~AltTabHighlightBorder();

    AltTabHighlightBorder(const AltTabHighlightBorder&) = delete;
    AltTabHighlightBorder& operator=(const AltTabHighlightBorder&) = delete;

    // Positions the glow directly on target's current screen rect,
    // renders it, and makes it visible. `owner`, when non-null, keeps
    // the glow directly in front of that window in the Z order instead
    // of in the topmost band -- only meaningful for an instance
    // constructed with alwaysOnTop=false (ignored otherwise, since a
    // topmost instance always belongs in the topmost band). Pass the
    // window the glow is meant to sit on top of, not a raw
    // SetWindowPos insertion point -- SetWindowPos's own
    // hWndInsertAfter places a window *behind* the handle you give it,
    // the opposite of "in front of", so this resolves the correct
    // insertion point (whatever currently sits directly in front of
    // `owner`) internally rather than exposing that inversion to
    // callers.
    void ShowAroundTarget(HWND target, HWND owner = nullptr);

    // Hides the glow.
    void Hide();

private:
    HWND window_ = nullptr;
    bool alwaysOnTop_ = true;
};

}  // namespace polish

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
    // class (e.g. a stack's active-tile ring), where the highlight
    // should behave like an ordinary window: covered by whatever the
    // user brings to the front, not floating above unrelated apps
    // forever.
    //
    // `owner`: passed straight through as CreateWindowExW's hWndParent
    // -- for a WS_POPUP window that makes this an *owned* window rather
    // than an unrelated top-level one, and Windows itself then maintains
    // everything a stack's per-tile ring needs for free: always above
    // its owner in Z order (no code here has to re-assert that -- a real,
    // confirmed bug in an earlier unowned version, where activating the
    // owner buried the ring behind it with nothing to bring it back),
    // hidden automatically when the owner is minimized and shown again
    // on restore, and destroyed automatically if the owner is destroyed
    // first. Pass nullptr (the default) for the real Alt+Tab overlay,
    // which has no single owner to attach to and relies on
    // alwaysOnTop's WS_EX_TOPMOST instead.
    explicit AltTabHighlightBorder(HINSTANCE instance, bool alwaysOnTop = true, HWND owner = nullptr);
    ~AltTabHighlightBorder();

    AltTabHighlightBorder(const AltTabHighlightBorder&) = delete;
    AltTabHighlightBorder& operator=(const AltTabHighlightBorder&) = delete;

    // Positions the glow directly on target's current screen rect,
    // renders it, and makes it visible. Z-order is just
    // alwaysOnTop_ ? HWND_TOPMOST : HWND_TOP -- an owned window (see the
    // constructor's `owner`) doesn't need, and can't usefully take,
    // anything more specific than that; Windows keeps it in front of its
    // owner on its own.
    void ShowAroundTarget(HWND target);

    // Hides the glow.
    void Hide();

private:
    HWND window_ = nullptr;
    bool alwaysOnTop_ = true;
    // The screen size (not position) ShowAroundTarget last rendered the
    // ring's content at -- {-1, -1} (never a real size) before the first
    // call and after Hide(), so the next ShowAroundTarget always does a
    // full render rather than comparing against stale content. The
    // ring's rendered pixels depend only on target *size* (DPI, corner
    // radii, which corners get the screen-edge radius), never position
    // -- when a new call's target is the same size, ShowAroundTarget
    // skips the DIB/GDI+/premultiply work entirely and just moves the
    // existing content, so a target that's only moving (e.g. the whole
    // stack being dragged by its title bar, which re-renders the ring on
    // every WM_MOVE) stays cheap.
    SIZE lastRenderedSize_{-1, -1};
};

}  // namespace polish

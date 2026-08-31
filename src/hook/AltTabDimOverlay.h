#pragma once

#include <windows.h>

namespace polish {

// A single, click-through, non-activating overlay that dims whatever's
// directly beneath it -- used to visually de-emphasize every Alt+Tab
// candidate window except the currently-highlighted one, so the user can
// spatially see which real window they're about to switch to, in its
// actual on-screen position, rather than parsing a thumbnail. Chosen
// over a thumbnail-grid popup specifically so the feedback is "is this
// the window I'm looking at" rather than "does this small preview look
// like the window I want."
//
// Deliberately NOT topmost: it's inserted just above its own target in
// the normal z-order (see ShowOverTarget), so any other window already
// stacked in front of the target is never incorrectly dimmed too.
class AltTabDimOverlay {
public:
    explicit AltTabDimOverlay(HINSTANCE instance);
    ~AltTabDimOverlay();

    AltTabDimOverlay(const AltTabDimOverlay&) = delete;
    AltTabDimOverlay& operator=(const AltTabDimOverlay&) = delete;

    // Positions the overlay exactly over target's current screen rect,
    // just above it in z-order, and makes it visible (dimmed).
    void ShowOverTarget(HWND target);

    // Hides the overlay, so target then appears normally (undimmed).
    void Hide();

private:
    HWND window_ = nullptr;
};

}  // namespace polish

#pragma once

#include <windows.h>

namespace polish {

// A thin, click-through, non-activating colored frame drawn just outside
// the currently-highlighted Alt+Tab candidate's own rect -- an active
// signal that this is the window you're about to switch to, rather than
// relying solely on "the one that isn't dimmed" (AltTabDimOverlay) to
// make that obvious. Uses SetWindowRgn to punch out its own interior, so
// it never covers the target's actual content -- only a thickness-wide
// frame around the outside is ever painted.
class AltTabHighlightBorder {
public:
    explicit AltTabHighlightBorder(HINSTANCE instance);
    ~AltTabHighlightBorder();

    AltTabHighlightBorder(const AltTabHighlightBorder&) = delete;
    AltTabHighlightBorder& operator=(const AltTabHighlightBorder&) = delete;

    // Positions the frame just outside target's current screen rect and
    // makes it visible.
    void ShowAroundTarget(HWND target);

    // Hides the frame.
    void Hide();

private:
    HWND window_ = nullptr;
};

}  // namespace polish

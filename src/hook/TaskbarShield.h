#pragma once

#include <windows.h>

#include <optional>
#include <vector>

#include "windowtracking/TaskbarButtons.h"

namespace polish {

// A nearly-invisible window laid exactly over the taskbar's app buttons,
// whose only job is to own the pointer in that strip so the native hover
// thumbnail flyout is never created.
//
// This is the one technique that works. Four others were built and
// measured first and all failed -- the registry's ExtendedUIHoverTime,
// covering the flyout with a topmost panel, raising a z-band with
// SetWindowBand, and swallowing WM_MOUSEMOVE in a low-level hook (which
// freezes the cursor outright). docs/LIMITATIONS.md #22 records all five
// in full, including why the three that look like they should work do
// not. Nothing here gates the input stream and nothing here has to
// out-draw the flyout: the taskbar simply never receives a pointer-enter,
// so its HoverFlyoutController never starts its dwell and there is no
// flyout to out-draw.
//
// One window per taskbar, not one spanning them all: a union rect across
// two monitors would cover the desktop between them. The buttons carry
// the taskbar they came from (TaskbarButton::taskbar), which is what this
// stacks on.
//
// Two constraints that are not style choices -- each was a real failure:
//
//   - Alpha must be 1, not 0. A fully transparent layered window is
//     excluded from hit-testing entirely, so at alpha 0 it would receive
//     nothing and therefore block nothing. 1/255 of black over the
//     taskbar is not perceptible. That is per-pixel alpha
//     (UpdateLayeredWindow), not a window-wide LWA_ALPHA, because the
//     same surface also draws the hover highlight -- see SetHighlight.
//   - The strip moves. Buttons shift when an app opens or closes, the
//     taskbar auto-hides, monitors change, and explorer restarts. Update
//     is expected to be called on every one of those.
class TaskbarShield {
public:
    explicit TaskbarShield(HINSTANCE instance);
    ~TaskbarShield();

    TaskbarShield(const TaskbarShield&) = delete;
    TaskbarShield& operator=(const TaskbarShield&) = delete;

    // Re-covers the app-button strip of every taskbar represented in
    // `buttons`, creating and destroying per-taskbar windows as monitors
    // come and go. An empty list hides everything -- a taskbar with no
    // app buttons has no strip worth shielding, and covering the
    // resulting empty rect would sit over the Start button.
    void Update(const std::vector<TaskbarButton>& buttons);

    // Whether the shield is currently letting the pointer through to the
    // real taskbar (WS_EX_TRANSPARENT on) or absorbing it (off).
    //
    // This is how everything the shield must NOT own gets back to the
    // taskbar: the right-click jumplist, shift/middle-click to open a new
    // instance, dragging onto a button, and the Ctrl escape hatch. The
    // caller turns it on the moment it knows an event is the taskbar's,
    // and off again once the gesture is over -- see TaskbarHook, which is
    // where that is known.
    //
    // WS_EX_TRANSPARENT, specifically, because it is the only pass-through
    // that crosses a process boundary. Answering HTTRANSPARENT from
    // WM_NCHITTEST was tried first and does not work: it passes the point
    // to underlying windows *in the same thread only*, so against
    // explorer's taskbar it absorbs exactly as HTCLIENT does. Measured
    // both ways against a live taskbar -- the shield's hit-test really did
    // return HTTRANSPARENT and the flyout really did stay away, which is
    // what makes that a convincing wrong answer rather than an obvious
    // one. With WS_EX_TRANSPARENT set, WindowFromPoint over the strip
    // returns MSTaskSwWClass and the flyout comes back.
    //
    // Cheap to call repeatedly: a call that does not change the style
    // returns without touching the window.
    void SetPassThrough(bool passThrough);
    bool IsPassThrough() const { return passThrough_; }

    // How many times the shield has been opened since the process
    // started. Only ever read for diagnostics: the shield being open is
    // what lets the native flyout back, so a report of the flyout
    // reappearing is answerable from the log by whether this moved.
    unsigned PassThroughOpenCount() const { return passThroughOpens_; }

    // Whether the shield actually owns `screenPoint` -- that is, whether
    // hit-testing there lands on it rather than on something above it.
    //
    // Not a rect check: the point of asking is that the shield can be
    // exactly where it should be, visible and opaque, and still be
    // bypassed because the taskbar has been raised above it. Opening the
    // Start menu does that permanently (docs/LIMITATIONS.md #24), and
    // nothing about the shield's own state reveals it.
    bool CoversPoint(POINT screenPoint) const;

    // Draws Polish's own hover highlight behind the button at
    // `buttonRect` (screen coordinates), or clears it when empty.
    //
    // It exists because the shield took the native one away. The taskbar
    // lights up the button under the pointer, and it does that by
    // receiving the pointer -- which is exactly what this window stops.
    // Nothing else can put it back: the highlight is drawn by explorer
    // inside its own XAML tree and there is no API to ask for it.
    //
    // Drawn by the shield rather than by a window of its own because the
    // shield is already exactly over the strip, already above the
    // taskbar, and in the UIAccess build already in the band that wins
    // while the Start menu is open (docs/UIACCESS.md). A second window
    // would have to be kept in step with all three.
    //
    // Cheap to call repeatedly: a call naming the same button as the last
    // one returns without repainting.
    void SetHighlight(std::optional<RECT> buttonRect);

    // Uncovers every strip, leaving the native taskbar completely
    // untouched. The tray toggle's off switch, and what the destructor
    // does implicitly.
    void Hide();

private:
    struct Shield {
        HMONITOR taskbar = nullptr;
        HWND window = nullptr;
        RECT rect{};  // last applied, to skip no-op SetWindowPos churn
        // Consecutive Updates in which this taskbar was absent from the
        // buttons. See Update: one absence is not evidence of anything.
        int missedUpdates = 0;
        // The size the current layered surface was rendered at, so a
        // reposition that does not resize the strip skips the repaint.
        SIZE renderedSize{};
        bool renderedHighlight = false;
        RECT renderedHighlightRect{};
    };

    HWND CreateShieldWindow();
    // Repaints one shield's layered surface: alpha 1 everywhere, plus the
    // highlight if it falls on this taskbar.
    void Render(Shield& shield);

    HINSTANCE instance_;
    std::vector<Shield> shields_;
    bool passThrough_ = false;
    unsigned passThroughOpens_ = 0;
    std::optional<RECT> highlight_;
};

}  // namespace polish

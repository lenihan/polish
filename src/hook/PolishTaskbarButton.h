#pragma once

#include <windows.h>

#include <functional>
#include <optional>

namespace polish {

// Polish's own button on the taskbar, drawn to sit beside Start the way
// Task View does.
//
// Windows 11 has no API for a third-party shell button, and the stand-in
// *app* window this replaced (a one-pixel WS_EX_APPWINDOW popup, which is
// how stacks appear on the taskbar) carries three things that are wrong
// for a button: a running-app indicator under it, an entry in Alt+Tab, and
// the app's own icon in the button's slot. So this is a plain Polish
// window laid over free taskbar space -- the technique TaskbarShield uses
// for its hover highlight -- that paints an icon and a hover fill, and
// handles its own clicks. It is a tool window, so it is in neither the
// taskbar nor any switcher.
//
// What that costs, accepted: it is only *drawn* like a native button.
// The hover fill is an approximation of the XAML control's (faint rounded
// rectangle, no animation), and while Start or Search is open the taskbar
// moves to a higher z-band that this window, in plain polish.exe, cannot
// beat (docs/LIMITATIONS.md #24) -- it is simply covered then, which is
// fine, since neither is a moment anyone wants this button.
//
// Where it goes is not this class's business: the owner computes a rect
// (windowtracking/TaskbarPlacement.h) and calls SetRect, with nullopt to
// hide it.
class PolishTaskbarButton {
public:
    struct Callbacks {
        // Left button released over the button. Run from the window's own
        // procedure: post rather than do real work here.
        std::function<void()> onClick;
        // Right button released over it.
        std::function<void()> onRightClick;
    };

    PolishTaskbarButton(HINSTANCE instance, Callbacks callbacks);
    ~PolishTaskbarButton();

    PolishTaskbarButton(const PolishTaskbarButton&) = delete;
    PolishTaskbarButton& operator=(const PolishTaskbarButton&) = delete;

    bool IsValid() const { return window_ != nullptr; }
    HWND Handle() const { return window_; }

    // Places the button over `rect` (screen, physical pixels) and shows
    // it, or hides it for nullopt. Re-asserts topmost every time, which is
    // cheap and is what keeps it above a taskbar that Explorer has just
    // raised.
    void SetRect(std::optional<RECT> rect);

    // Puts it back above the taskbar without moving it. For the moment
    // Explorer raises the taskbar over it (a click on the taskbar).
    void ReassertTopmost();

    // Drawn pressed while true: for as long as the panel it opened is up.
    void SetActive(bool active);

    bool Visible() const { return visible_; }
    // The button's current rect; empty while hidden.
    RECT Rect() const { return visible_ ? rect_ : RECT{}; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Render();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND tooltip_ = nullptr;
    RECT rect_{};
    bool visible_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    bool active_ = false;
    bool trackingLeave_ = false;
    // What was last rendered, so an unchanged state costs nothing.
    SIZE renderedSize_{};
    UINT renderedDpi_ = 0;
    int renderedState_ = -1;
    bool renderedDark_ = false;
    Callbacks callbacks_;
};

}  // namespace polish

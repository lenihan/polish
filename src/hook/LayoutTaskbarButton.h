#pragma once

#include <windows.h>

#include <functional>

namespace polish {

// Polish's own taskbar button, labelled "Layout".
//
// The native taskbar cannot be given a button, so this is the same trick a
// stack uses (see StackStripWindow's taskbar stand-in): a one-pixel,
// off-screen, never-painted window with WS_EX_APPWINDOW and an
// AppUserModelID of its own, which the shell lists like any running app.
// What makes it a *button* rather than a window is what a click on it means:
// the shell activates the window, and that activation is the signal.
//
// The owner is told with `onClicked`, and is expected to hand the foreground
// straight back to whatever the user was in and open its menu. The stand-in
// is never meant to stay active; if it did, the user would be "in" a window
// that does not exist.
//
// Deliberately not WS_EX_NOACTIVATE: the shell would not activate it, and
// there would be no click to see.
class LayoutTaskbarButton {
public:
    // `onClicked` runs from the window's own procedure; the owner should
    // post rather than do real work there (activating another window from
    // inside this one's WM_ACTIVATE is asking for a nested activation).
    // `onCloseRequested` is the button's "Close window" jump-list entry.
    LayoutTaskbarButton(HINSTANCE instance, std::function<void()> onClicked,
                        std::function<void()> onCloseRequested);
    ~LayoutTaskbarButton();

    LayoutTaskbarButton(const LayoutTaskbarButton&) = delete;
    LayoutTaskbarButton& operator=(const LayoutTaskbarButton&) = delete;

    bool IsValid() const { return window_ != nullptr; }
    HWND Handle() const { return window_; }

    // The AppUserModelID, which is also how the taskbar's own button list
    // (read through UI Automation) identifies this button.
    static const wchar_t* AppId();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND window_ = nullptr;
    std::function<void()> onClicked_;
    std::function<void()> onCloseRequested_;
};

}  // namespace polish

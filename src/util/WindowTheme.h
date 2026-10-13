#pragma once

#include <windows.h>

namespace polish {

// Light/dark theming for one of Polish's own plain Win32 windows (the
// Settings window, the hotkey dialog): follows the system's app theme, and
// does nothing at all in light mode, leaving the window as it always was.
//
// Usage: call Apply() when the window is shown (it re-reads the system
// setting, so a change made while Polish runs is picked up), and offer
// every message to Handle() first from the window procedure:
//
//   LRESULT result = 0;
//   if (theme_.Handle(hwnd, message, wParam, &result)) return result;
class WindowTheme {
public:
    WindowTheme() = default;
    ~WindowTheme();

    WindowTheme(const WindowTheme&) = delete;
    WindowTheme& operator=(const WindowTheme&) = delete;

    // Themes the title bar and every child control of `window`, and
    // repaints it. Controls must already exist.
    void Apply(HWND window);

    bool dark() const { return dark_; }

    // Background and control colours for dark mode. True if the message was
    // handled and `*result` is the window procedure's return value.
    bool Handle(HWND window, UINT message, WPARAM wParam, LRESULT* result);

private:
    bool dark_ = false;
    HBRUSH background_ = nullptr;
    HBRUSH editBackground_ = nullptr;
};

}  // namespace polish

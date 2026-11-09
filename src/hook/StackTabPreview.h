#pragma once

#include <windows.h>

#include <dwmapi.h>

namespace polish {

// The thumbnail shown when the pointer rests on a stack tab whose window is
// not the one in front: a live DWM thumbnail of that window, so you can see
// what a tab holds without raising it.
//
// A tool window and no-activate and click-through, so it is never in Alt+Tab,
// never takes focus and never intercepts the tab it is describing. It appears
// after a short dwell rather than at once, so sweeping the pointer along the
// strip does not flicker one preview per tab.
//
// This is the stack's own affordance. The Alt+Tab and taskbar lists stay icon
// and title only (a decision recorded in the project notes); a stack's tab
// carries nothing but an icon and a title, which is the case for a picture.
class StackTabPreview {
public:
    explicit StackTabPreview(HINSTANCE instance);
    ~StackTabPreview();

    StackTabPreview(const StackTabPreview&) = delete;
    StackTabPreview& operator=(const StackTabPreview&) = delete;

    // Shows `source` below the tab at `tabRect` (screen pixels) once the dwell
    // has passed, or beside it when `beside` (a strip down the stack's left
    // side). Asking again for the same source just keeps the pending show.
    void Request(HWND source, const RECT& tabRect, bool beside, UINT dpi);

    // Withdraws a pending or visible preview.
    void Cancel();

    HWND Handle() const { return window_; }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    void ShowNow();
    void Unregister();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;

    HWND pendingSource_ = nullptr;
    RECT pendingTab_{};
    bool pendingBeside_ = false;
    UINT pendingDpi_ = 96;
};

}  // namespace polish

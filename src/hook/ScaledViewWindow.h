#pragma once

#include <windows.h>

#include <dwmapi.h>

#include <functional>

namespace polish {

// The small window that stands in for a scaled one: a live DWM thumbnail
// of the real window, which is parked (see WindowParking) and so neither
// visible nor clickable itself.
//
// A tool window and no-activate, so it is never in Alt+Tab and never takes
// focus -- the real window keeps its own taskbar button and its place in
// the switchers, and it is the *real* window that activation restores.
//
// Input, deliberately minimal:
//   click (no drag)        -> restore the window to 1:1 where the view is
//   drag the body          -> move the view
//   drag a corner          -> rescale, aspect locked, snapping to presets
//   right-click            -> menu: 1:1, presets, always on top, close
// Nothing is forwarded into the window: while it is scaled you watch it,
// and a click is how you say "now I need it".
//
// The thumbnail paints over the window's own client area, so there is
// nothing the view can draw on top of it; the 1px accent border is the
// window background showing around the inset thumbnail.
class ScaledViewWindow {
public:
    struct Callbacks {
        // Click, or a corner dragged up to full size.
        std::function<void()> onRestore;
        // A move or resize gesture finished; `rect` is the final view rect.
        std::function<void(const RECT& rect)> onViewChanged;
        // Menu: scale to a preset, keeping the view's top-left.
        std::function<void(double scale)> onSetScale;
        // Menu: ask the real window to close.
        std::function<void()> onClose;
        // Menu: the "always on top" box was toggled.
        std::function<void(bool topmost)> onTopmostChanged;
    };

    // `source` is the real window; `realSize` its visible size at 1:1.
    ScaledViewWindow(HINSTANCE instance, HWND source, SIZE realSize, const RECT& viewRect, bool topmost,
                     Callbacks callbacks);
    ~ScaledViewWindow();

    ScaledViewWindow(const ScaledViewWindow&) = delete;
    ScaledViewWindow& operator=(const ScaledViewWindow&) = delete;

    // False if the thumbnail could not be registered (the source is gone).
    bool IsValid() const { return window_ != nullptr && thumbnail_ != nullptr; }

    void SetRect(const RECT& viewRect);
    void SetRealSize(SIZE realSize);
    void SetTopmost(bool topmost);
    void Show();
    void Hide();
    bool Visible() const { return visible_; }

    HWND Handle() const { return window_; }
    const RECT& Rect() const { return rect_; }
    HWND Source() const { return source_; }

    // The window class's procedure; public only so the class registration
    // in the .cpp can name it.
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void UpdateThumbnail();
    void ApplyRect(const RECT& rect);
    void ShowMenu();
    int CornerAt(POINT clientPoint, int* cornerOut) const;

    HWND window_ = nullptr;
    HWND source_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;
    SIZE realSize_{};
    RECT rect_{};
    bool topmost_ = true;
    bool visible_ = false;
    Callbacks callbacks_;

    // Gesture state.
    enum class Drag { None, Pending, Move, Resize };
    Drag drag_ = Drag::None;
    int dragCorner_ = 0;
    POINT dragStartCursor_{};
    RECT dragStartRect_{};
};

}  // namespace polish

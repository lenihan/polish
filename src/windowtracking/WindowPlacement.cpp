#include "windowtracking/WindowPlacement.h"

#include <algorithm>

#include "windowtracking/RectUtils.h"

namespace polish {

RECT VisibleRectAndInset(HWND hwnd, RECT& insetOut) {
    RECT windowRect{};
    GetWindowRect(hwnd, &windowRect);
    RECT visible{};
    if (!GetVisibleWindowRect(hwnd, visible)) {
        visible = windowRect;
    }
    insetOut = {windowRect.left - visible.left, windowRect.top - visible.top, windowRect.right - visible.right,
                windowRect.bottom - visible.bottom};
    return visible;
}

SIZE MinimumVisibleSizeFor(HWND hwnd) {
    MINMAXINFO info{};
    DWORD_PTR ignored = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&info),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &ignored) == 0) {
        return SIZE{0, 0};
    }
    RECT inset{};
    VisibleRectAndInset(hwnd, inset);
    // windowRect = visible + inset, so visible size = window size - (right - left) etc.
    const bool maximized = IsZoomed(hwnd) != FALSE;
    const LONG borderX = maximized ? 0 : inset.right - inset.left;
    const LONG borderY = maximized ? 0 : inset.bottom - inset.top;
    return SIZE{std::max<LONG>(0, info.ptMinTrackSize.x - borderX),
                std::max<LONG>(0, info.ptMinTrackSize.y - borderY)};
}

void PlaceWindowVisible(HWND hwnd, const RECT& visible) {
    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    RECT inset{};
    VisibleRectAndInset(hwnd, inset);
    const RECT window{visible.left + inset.left, visible.top + inset.top, visible.right + inset.right,
                      visible.bottom + inset.bottom};
    SetWindowPos(hwnd, nullptr, window.left, window.top, window.right - window.left, window.bottom - window.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

}  // namespace polish

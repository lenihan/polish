#include "hook/GroupTabThumbnail.h"

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupTabThumbnail";
constexpr int kWidth = 240;   // logical (96 DPI) px
constexpr int kHeight = 160;  // logical px

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    if (message == WM_ERASEBKGND) {
        // Same fix as GroupChromeWindow's own WM_ERASEBKGND override, for
        // the same reason: the class's default background brush
        // (COLOR_WINDOW, i.e. white) would otherwise paint on every
        // erase -- visibly for a frame -- before WM_PAINT's StretchBlt
        // draws the actual snapshot over it. WM_PAINT already fully
        // repaints the client area on its own every time, so the
        // default erase step here is pure overhead. A genuine
        // improvement on its own, but not what a "flashes ~3 times on
        // hover" user report traced back to -- that was the chrome
        // window missing WS_CLIPCHILDREN (see GroupChromeWindow::Show).
        return 1;
    }
    if (message == WM_PAINT) {
        auto* snapshotPtr = reinterpret_cast<HBITMAP*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);

        if (snapshotPtr != nullptr && *snapshotPtr != nullptr) {
            HDC memDC = CreateCompatibleDC(hdc);
            HGDIOBJ oldBitmap = SelectObject(memDC, *snapshotPtr);
            BITMAP bitmapInfo{};
            GetObject(*snapshotPtr, sizeof(bitmapInfo), &bitmapInfo);
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, 0, 0, client.right, client.bottom, memDC, 0, 0, bitmapInfo.bmWidth,
                       bitmapInfo.bmHeight, SRCCOPY);
            SelectObject(memDC, oldBitmap);
            DeleteDC(memDC);
        } else {
            HBRUSH background = CreateSolidBrush(RGB(0xF0, 0xF0, 0xF0));
            FillRect(hdc, &client, background);
            DeleteObject(background);
        }

        HBRUSH borderBrush = CreateSolidBrush(RGB(0x40, 0x40, 0x40));
        FrameRect(hdc, &client, borderBrush);
        DeleteObject(borderBrush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

GroupTabThumbnail::GroupTabThumbnail(HINSTANCE instance) : instance_(instance) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
}

GroupTabThumbnail::~GroupTabThumbnail() {
    Hide();
    if (snapshot_ != nullptr) {
        DeleteObject(snapshot_);
    }
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void GroupTabThumbnail::ShowFor(HBITMAP snapshot, const RECT& tabScreenRect, bool preferRightSide) {
    if (window_ == nullptr) {
        // WS_EX_NOACTIVATE so hovering a tab never steals focus from
        // whatever the user is actually working in. GWLP_USERDATA is
        // set to &snapshot_ (a stable address for this object's
        // lifetime) via lpCreateParams so WM_PAINT can always paint
        // whatever the *current* snapshot is, not a stale copy.
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWindowClassName, L"",
                                   WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, &snapshot_);
    }
    if (window_ == nullptr) {
        return;
    }

    // Copied into this class's own bitmap rather than displaying
    // `snapshot` directly -- the caller (GroupManager's cache) may
    // replace or free its copy the next time that member's thumbnail is
    // recaptured, which could otherwise leave this popup holding a
    // dangling handle while still visible.
    if (snapshot != nullptr) {
        BITMAP bitmapInfo{};
        if (GetObjectW(snapshot, sizeof(bitmapInfo), &bitmapInfo) != 0 && bitmapInfo.bmWidth > 0 &&
            bitmapInfo.bmHeight > 0) {
            HDC screenDC = GetDC(nullptr);
            HDC srcDC = CreateCompatibleDC(screenDC);
            HDC dstDC = CreateCompatibleDC(screenDC);
            HBITMAP freshCopy = CreateCompatibleBitmap(screenDC, bitmapInfo.bmWidth, bitmapInfo.bmHeight);
            HGDIOBJ oldSrc = SelectObject(srcDC, snapshot);
            HGDIOBJ oldDst = SelectObject(dstDC, freshCopy);
            BitBlt(dstDC, 0, 0, bitmapInfo.bmWidth, bitmapInfo.bmHeight, srcDC, 0, 0, SRCCOPY);
            SelectObject(srcDC, oldSrc);
            SelectObject(dstDC, oldDst);
            DeleteDC(srcDC);
            DeleteDC(dstDC);
            ReleaseDC(nullptr, screenDC);

            if (snapshot_ != nullptr) {
                DeleteObject(snapshot_);
            }
            snapshot_ = freshCopy;
        }
    }

    const UINT dpi = GetDpiForWindow(window_) != 0 ? GetDpiForWindow(window_) : GetDpiForSystem();
    const int width = Scale(kWidth, dpi);
    const int height = Scale(kHeight, dpi);

    // Centered under the tab (Horizontal alignment) or centered to its
    // right (Vertical), per the caller-supplied `preferRightSide` --
    // the tab rect's own shape can't tell these apart (a Vertical tab
    // row is a fixed-width column that's wide and short, not narrow
    // and tall). "Under" a specific vertical tab row would risk
    // overlapping the next row down, which is why Vertical needs its
    // own branch rather than reusing the horizontal formula as-is.
    int x;
    int y;
    if (preferRightSide) {
        x = tabScreenRect.right + Scale(4, dpi);
        y = tabScreenRect.top + ((tabScreenRect.bottom - tabScreenRect.top) - height) / 2;
    } else {
        x = tabScreenRect.left + ((tabScreenRect.right - tabScreenRect.left) - width) / 2;
        y = tabScreenRect.bottom + Scale(4, dpi);
    }
    // Clamped to the tab's own monitor's work area so it can't be
    // positioned partly off-screen near an edge.
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(MonitorFromRect(&tabScreenRect, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        const RECT& workArea = monitorInfo.rcWork;
        x = std::clamp(x, static_cast<int>(workArea.left), static_cast<int>(workArea.right) - width);
        y = std::clamp(y, static_cast<int>(workArea.top), static_cast<int>(workArea.bottom) - height);
    }

    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    InvalidateRect(window_, nullptr, TRUE);
    UpdateWindow(window_);
}

void GroupTabThumbnail::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

}  // namespace polish

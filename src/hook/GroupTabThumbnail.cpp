#include "hook/GroupTabThumbnail.h"

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupTabThumbnail";
constexpr int kWidth = 240;   // logical (96 DPI) px
constexpr int kHeight = 160;  // logical px

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

// PW_RENDERFULLCONTENT (Windows 8.1+) -- captures a window's actual
// rendered content (including hardware-accelerated/DirectComposition
// surfaces a plain BitBlt-based capture can't see), which the plain
// PW_CLIENTONLY-only flag alone doesn't guarantee on every app.
constexpr UINT kPrintWindowRenderFullContent = 0x00000002;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
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

void GroupTabThumbnail::ShowFor(HWND member, const RECT& tabScreenRect) {
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

    RECT memberClient{};
    GetClientRect(member, &memberClient);
    const int memberWidth = memberClient.right - memberClient.left;
    const int memberHeight = memberClient.bottom - memberClient.top;
    if (memberWidth > 0 && memberHeight > 0) {
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP freshSnapshot = CreateCompatibleBitmap(screenDC, memberWidth, memberHeight);
        HGDIOBJ oldBitmap = SelectObject(memDC, freshSnapshot);
        const BOOL captured = PrintWindow(member, memDC, kPrintWindowRenderFullContent);
        SelectObject(memDC, oldBitmap);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);

        if (captured) {
            if (snapshot_ != nullptr) {
                DeleteObject(snapshot_);
            }
            snapshot_ = freshSnapshot;
        } else {
            DeleteObject(freshSnapshot);
        }
    }

    const UINT dpi = GetDpiForWindow(window_) != 0 ? GetDpiForWindow(window_) : GetDpiForSystem();
    const int width = Scale(kWidth, dpi);
    const int height = Scale(kHeight, dpi);

    // Centered under the tab, clamped to the tab's own monitor's work
    // area so it can't be positioned partly off-screen near an edge.
    int x = tabScreenRect.left + ((tabScreenRect.right - tabScreenRect.left) - width) / 2;
    int y = tabScreenRect.bottom + Scale(4, dpi);
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

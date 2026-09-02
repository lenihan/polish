#include "hook/GroupTabThumbnail.h"

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kWindowClassName[] = L"PolishGroupTabThumbnail";
constexpr int kWidth = 240;   // logical (96 DPI) px
constexpr int kHeight = 160;  // logical px

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
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
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void GroupTabThumbnail::ShowFor(HWND member, const RECT& tabScreenRect) {
    if (window_ == nullptr) {
        // WS_EX_NOACTIVATE so hovering a tab never steals focus from
        // whatever the user is actually working in; a plain, non-
        // layered WS_POPUP is enough for DwmRegisterThumbnail (already
        // confirmed working this way during the original Alt+Tab
        // research -- no WS_EX_LAYERED needed for a DWM thumbnail
        // specifically, unlike this app's dim overlay/highlight border,
        // which paint their own content instead of hosting one).
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWindowClassName, L"",
                                   WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, nullptr);
    }
    if (window_ == nullptr) {
        return;
    }

    if (thumbnail_ != nullptr) {
        DwmUnregisterThumbnail(thumbnail_);
        thumbnail_ = nullptr;
    }

    const UINT dpi = GetDpiForWindow(window_) != 0 ? GetDpiForWindow(window_) : GetDpiForSystem();
    const int width = Scale(kWidth, dpi);
    const int height = Scale(kHeight, dpi);

    // Centered under the tab, clamped to the tab's own monitor's work
    // area so it can't be positioned partly off-screen near an edge.
    int x = tabScreenRect.left + ((tabScreenRect.right - tabScreenRect.left) - width) / 2;
    int y = tabScreenRect.bottom + Scale(4, dpi);
    RECT workArea{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(MonitorFromRect(&tabScreenRect, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        workArea = monitorInfo.rcWork;
        x = std::clamp(x, static_cast<int>(workArea.left), static_cast<int>(workArea.right) - width);
        y = std::clamp(y, static_cast<int>(workArea.top), static_cast<int>(workArea.bottom) - height);
    }

    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    ShowWindow(window_, SW_SHOWNOACTIVATE);

    if (DwmRegisterThumbnail(window_, member, &thumbnail_) == S_OK) {
        RECT client{};
        GetClientRect(window_, &client);
        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        props.rcDestination = client;
        props.fVisible = TRUE;
        props.opacity = 255;
        props.fSourceClientAreaOnly = TRUE;
        DwmUpdateThumbnailProperties(thumbnail_, &props);
    }
}

void GroupTabThumbnail::Hide() {
    if (thumbnail_ != nullptr) {
        DwmUnregisterThumbnail(thumbnail_);
        thumbnail_ = nullptr;
    }
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

}  // namespace polish

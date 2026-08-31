#include "hook/AltTabHighlightBorder.h"

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishAltTabHighlightBorder";

// Windows accent blue -- matches resources/polish.ico's sparkle color.
constexpr COLORREF kBorderColor = RGB(0, 120, 212);

LRESULT CALLBACK BorderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        RECT client;
        GetClientRect(hwnd, &client);
        HBRUSH brush = CreateSolidBrush(kBorderColor);
        FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
        DeleteObject(brush);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BorderProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

AltTabHighlightBorder::AltTabHighlightBorder(HINSTANCE instance) {
    EnsureClassRegistered(instance);
    // WS_EX_TOPMOST here (unlike AltTabDimOverlay's deliberate avoidance
    // of it) is a reasonable simplification: this is a thin decorative
    // frame sitting just outside the target's own rect, not a large
    // rect that could plausibly cover another window's real content the
    // way a full-window dim overlay could.
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                               kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (window_ != nullptr) {
        SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
    }
}

AltTabHighlightBorder::~AltTabHighlightBorder() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void AltTabHighlightBorder::ShowAroundTarget(HWND target) {
    if (window_ == nullptr) {
        return;
    }
    RECT targetRect;
    if (!GetWindowRect(target, &targetRect)) {
        return;
    }

    const UINT dpi = GetDpiForWindow(target);
    const int thickness = MulDiv(6, static_cast<int>(dpi), 96);

    RECT outer = targetRect;
    InflateRect(&outer, thickness, thickness);
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;

    SetWindowPos(window_, HWND_TOPMOST, outer.left, outer.top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Punch out the interior so only the frame itself ever paints --
    // target's own content, directly beneath the hole, is never covered.
    HRGN outerRgn = CreateRectRgn(0, 0, width, height);
    HRGN innerRgn = CreateRectRgn(thickness, thickness, width - thickness, height - thickness);
    CombineRgn(outerRgn, outerRgn, innerRgn, RGN_DIFF);
    if (!SetWindowRgn(window_, outerRgn, TRUE)) {
        DeleteObject(outerRgn);  // ownership only transfers on success
    }
    DeleteObject(innerRgn);
}

void AltTabHighlightBorder::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

}  // namespace polish

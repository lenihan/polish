#include "hook/AltTabDimOverlay.h"

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishAltTabDimOverlay";

// Out of 255; tuned by eye, not a considered/final value -- easy to
// adjust once this is actually visible on screen.
constexpr BYTE kDimAlpha = 140;

LRESULT CALLBACK DimOverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client,
                  reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
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
    wc.lpfnWndProc = DimOverlayProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

AltTabDimOverlay::AltTabDimOverlay(HINSTANCE instance) {
    EnsureClassRegistered(instance);
    // WS_EX_TRANSPARENT: click-through, since this is a keyboard-only
    // gesture and the overlay should never intercept a mouse event.
    // WS_EX_NOACTIVATE: never steals foreground/focus. Deliberately NOT
    // WS_EX_TOPMOST -- see ShowOverTarget for why: this overlay's
    // z-order is managed explicitly, per-show, relative to its own
    // target only.
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP,
                               0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (window_ != nullptr) {
        SetLayeredWindowAttributes(window_, 0, kDimAlpha, LWA_ALPHA);
    }
}

AltTabDimOverlay::~AltTabDimOverlay() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void AltTabDimOverlay::ShowOverTarget(HWND target) {
    if (window_ == nullptr) {
        return;
    }
    RECT rect;
    if (!GetWindowRect(target, &rect)) {
        return;
    }
    // Inserted directly above target in the *normal* z-order, not
    // WS_EX_TOPMOST -- a topmost overlay would blindly cover target's
    // full rect regardless of what's actually rendered there, incorrectly
    // dimming any other window (candidate or not) that happens to be
    // stacked in front of target on the real desktop. SetWindowPos's
    // hWndInsertAfter places a window *behind* the given handle (a
    // documented gotcha elsewhere in this codebase), so landing directly
    // above target means inserting "after" whatever is currently
    // directly above target -- or HWND_TOP if nothing is.
    const HWND aboveTarget = GetWindow(target, GW_HWNDPREV);
    SetWindowPos(window_, aboveTarget != nullptr ? aboveTarget : HWND_TOP, rect.left, rect.top,
                 rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void AltTabDimOverlay::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

}  // namespace polish

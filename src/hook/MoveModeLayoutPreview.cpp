#include "hook/MoveModeLayoutPreview.h"

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishMoveModeLayoutPreview";

// One alpha for the whole panel -- see the class comment for why it is
// flat. High enough to be unmistakable on any wallpaper, low enough that
// the windows underneath still read, since the point is to show you
// where this one is going to land among them.
constexpr int kPanelAlpha = 110;

// How wide the border is, in DIPs. Generous: it is the part that says
// "this exact rectangle", and at a half or a quarter the edge is the
// only thing distinguishing one layout from another.
constexpr int kBorderDip = 4;

// The accent, lightened towards white for the border so it separates
// from the fill under a single flat alpha.
COLORREF Lighten(COLORREF color, int percent) {
    const auto mix = [percent](uint32_t channel) {
        const uint32_t lifted = channel + ((255u - channel) * static_cast<uint32_t>(percent)) / 100u;
        return std::min<uint32_t>(255u, lifted);
    };
    return mix(color & 0xFFu) | (mix((color >> 8) & 0xFFu) << 8) | (mix((color >> 16) & 0xFFu) << 16);
}

struct PaintState {
    COLORREF accent = 0;
    int borderPx = 0;
};

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) {
        auto* state = reinterpret_cast<PaintState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (state == nullptr) {
            return 1;
        }
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH fill = CreateSolidBrush(state->accent);
        FillRect(dc, &client, fill);
        DeleteObject(fill);
        HBRUSH border = CreateSolidBrush(Lighten(state->accent, 45));
        for (int i = 0; i < state->borderPx; ++i) {
            FrameRect(dc, &client, border);
            InflateRect(&client, -1, -1);
        }
        DeleteObject(border);
        return 1;
    }
    if (message == WM_NCDESTROY) {
        delete reinterpret_cast<PaintState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

MoveModeLayoutPreview::MoveModeLayoutPreview(HINSTANCE instance) {
    EnsureClassRegistered(instance);
    const DWORD exStyle =
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    window_ = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                              nullptr);
    if (window_ != nullptr) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new PaintState{}));
        SetLayeredWindowAttributes(window_, 0, static_cast<BYTE>(kPanelAlpha), LWA_ALPHA);
    }
}

MoveModeLayoutPreview::~MoveModeLayoutPreview() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
    }
}

void MoveModeLayoutPreview::Show(const RECT& rect, COLORREF accent, UINT dpi) {
    if (window_ == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        Hide();
        return;
    }
    const bool sameRect = visible_ && lastRect_.left == rect.left && lastRect_.top == rect.top &&
                          lastRect_.right == rect.right && lastRect_.bottom == rect.bottom;
    if (sameRect && lastAccent_ == accent && lastDpi_ == dpi) {
        return;
    }
    auto* state = reinterpret_cast<PaintState*>(GetWindowLongPtrW(window_, GWLP_USERDATA));
    if (state != nullptr) {
        state->accent = accent;
        state->borderPx = std::max(1, MulDiv(kBorderDip, static_cast<int>(dpi), 96));
    }
    SetWindowPos(window_, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, TRUE);
    UpdateWindow(window_);
    lastRect_ = rect;
    lastAccent_ = accent;
    lastDpi_ = dpi;
    visible_ = true;
}

void MoveModeLayoutPreview::Hide() {
    if (window_ != nullptr && visible_) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
}

}  // namespace polish

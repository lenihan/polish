#include "hook/PolishTaskbarButton.h"

#include <commctrl.h>

#include <algorithm>
#include <utility>

#include "hook/TaskbarVisuals.h"
#include "resource.h"
#include "util/Logging.h"

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishTaskbarButton";

// Out of 255, and it must not be 0: a fully transparent layered pixel is
// excluded from hit-testing, and this window is nothing but a hit target
// apart from its icon and fill. Same reasoning as TaskbarShield.
constexpr BYTE kBackgroundAlpha = 1;

// Win11's taskbar glyphs are 24 DIP in a 44x48 cell.
constexpr int kIconDip = 24;

// The pressed fill is fainter than hover, as the native taskbar draws it.
constexpr float kHoverStrength = 1.0f;
constexpr float kPressedStrength = 0.6f;

void EnsureClassRegistered(HINSTANCE instance, WNDPROC proc) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

LRESULT CALLBACK PolishTaskbarButton::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    PolishTaskbarButton* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<PolishTaskbarButton*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // CreateWindowEx has not returned, so window_ is still null and the
        // creation messages' default handling needs it.
        self->window_ = hwnd;
    } else {
        self = reinterpret_cast<PolishTaskbarButton*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

PolishTaskbarButton::PolishTaskbarButton(HINSTANCE instance, Callbacks callbacks)
    : instance_(instance), callbacks_(std::move(callbacks)) {
    EnsureClassRegistered(instance, WindowProc);
    // WS_EX_TOOLWINDOW keeps it out of Alt+Tab and off the taskbar proper;
    // WS_EX_NOACTIVATE so a click never takes the foreground from the
    // user's window; WS_EX_TOPMOST to sit above the taskbar, as the shield
    // does (a plain topmost window beats Shell_TrayWnd).
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName,
                              L"Polish", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        LogDebug(L"[Polish] Button: window creation failed");
        return;
    }
    // A tooltip, driven by the window's own mouse messages (TTF_SUBCLASS).
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr,
                               instance, nullptr);
    if (tooltip_ != nullptr) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = 1;
        GetClientRect(window_, &tool.rect);
        static wchar_t text[] = L"Polish";
        tool.lpszText = text;
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

PolishTaskbarButton::~PolishTaskbarButton() {
    if (window_ != nullptr) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
    }
}

void PolishTaskbarButton::ReassertTopmost() {
    if (window_ != nullptr && visible_) {
        SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void PolishTaskbarButton::SetActive(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    Render();
}

void PolishTaskbarButton::SetRect(std::optional<RECT> rect) {
    if (window_ == nullptr) {
        return;
    }
    if (!rect.has_value() || rect->right <= rect->left || rect->bottom <= rect->top) {
        if (visible_) {
            ShowWindow(window_, SW_HIDE);
            visible_ = false;
            hovered_ = false;
            pressed_ = false;
        }
        return;
    }
    rect_ = *rect;
    Render();  // positions the window as part of UpdateLayeredWindow
    SetWindowPos(window_, HWND_TOPMOST, rect_.left, rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    if (tooltip_ != nullptr) {
        // The tool's rect is in client coordinates and tracks the size.
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = window_;
        tool.uId = 1;
        tool.rect = RECT{0, 0, rect_.right - rect_.left, rect_.bottom - rect_.top};
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void PolishTaskbarButton::Render() {
    if (window_ == nullptr) {
        return;
    }
    const int width = rect_.right - rect_.left;
    const int height = rect_.bottom - rect_.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(window_));
    const bool dark = TaskbarIsDark();
    const int state = pressed_ || active_ ? 2 : (hovered_ ? 1 : 0);
    if (renderedSize_.cx == width && renderedSize_.cy == height && renderedDpi_ == dpi && renderedState_ == state &&
        renderedDark_ == dark) {
        // Nothing about the surface changes; still reposition, since the
        // caller may have moved it.
        POINT destination{rect_.left, rect_.top};
        UpdateLayeredWindow(window_, nullptr, &destination, nullptr, nullptr, nullptr, 0, nullptr, 0);
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        return;
    }

    // Everything at alpha 1: invisible, but in the hit-test.
    BYTE* pixels = static_cast<BYTE*>(bits);
    for (int i = 0; i < width * height; ++i) {
        pixels[i * 4 + 0] = 0;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = kBackgroundAlpha;
    }

    EnsureGdiplusStarted();
    {
        Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppARGB, pixels);
        Gdiplus::Graphics graphics(&bitmap);
        if (state > 0) {
            DrawTaskbarFill(graphics, Gdiplus::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
                            dpi, state == 2 ? kPressedStrength : kHoverStrength);
        }
        // The icon, centred in the cell. Pressed nudges it down a pixel
        // and in a little, the way the native buttons do.
        const int iconSize = MulDiv(kIconDip, static_cast<int>(dpi), 96) - (state == 2 ? 2 : 0);
        HICON icon = nullptr;
        if (SUCCEEDED(LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(IDI_POLISH_TRAY), iconSize, iconSize, &icon)) &&
            icon != nullptr) {
            Gdiplus::Bitmap glyph(icon);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.DrawImage(&glyph, (width - iconSize) / 2, (height - iconSize) / 2 + (state == 2 ? 1 : 0),
                               iconSize, iconSize);
            DestroyIcon(icon);
        }
    }
    PremultiplyAlpha(pixels, width * height);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, dib));
    POINT source{0, 0};
    SIZE size{width, height};
    POINT destination{rect_.left, rect_.top};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screenDC, &destination, &size, memDC, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    DeleteObject(dib);

    renderedSize_ = size;
    renderedDpi_ = dpi;
    renderedState_ = state;
    renderedDark_ = dark;
}

LRESULT PolishTaskbarButton::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!trackingLeave_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                trackingLeave_ = true;
            }
            if (!hovered_) {
                hovered_ = true;
                Render();
            }
            return 0;
        case WM_MOUSELEAVE:
            trackingLeave_ = false;
            if (hovered_ || pressed_) {
                hovered_ = false;
                pressed_ = false;
                Render();
            }
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = true;
            SetCapture(window_);
            Render();
            return 0;
        case WM_LBUTTONUP: {
            const bool wasPressed = pressed_;
            pressed_ = false;
            if (GetCapture() == window_) {
                ReleaseCapture();
            }
            Render();
            // A click is down and up on the button: releasing elsewhere
            // after pressing here is the user changing their mind.
            POINT point{static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                        static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
            RECT client{0, 0, rect_.right - rect_.left, rect_.bottom - rect_.top};
            if (wasPressed && PtInRect(&client, point) && callbacks_.onClick) {
                callbacks_.onClick();
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed_) {
                pressed_ = false;
                Render();
            }
            return 0;
        case WM_RBUTTONUP:
            if (callbacks_.onRightClick) {
                callbacks_.onRightClick();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace polish

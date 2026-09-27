#include "hook/TaskbarShield.h"

// objidl.h before gdiplus.h: GDI+ headers use IStream and friends
// without declaring them, and this project builds WIN32_LEAN_AND_MEAN.
#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>

#include "windowtracking/TaskbarReadPolicy.h"

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishTaskbarShield";

// Out of 255, and it must not be 0 -- see the class comment. This is the
// smallest value that still leaves the window in the hit-test.
constexpr BYTE kShieldAlpha = 1;

// The hover highlight, in logical pixels, scaled to the monitor's DPI at
// paint time. Sized against a Windows 11 button (44x48 logical): the fill
// sits a few pixels inside the button on every side, the way the native
// one does, rather than filling the whole cell edge to edge.
constexpr int kHighlightInsetXDip = 4;
constexpr int kHighlightInsetYDip = 5;
constexpr int kHighlightRadiusDip = 6;

// How strongly the fill reads against the taskbar. Deliberately faint:
// this is "the pointer is here", not a selection. Light-on-dark and
// dark-on-light, matching whichever way the taskbar itself is painted.
constexpr BYTE kHighlightAlphaDark = 30;
constexpr BYTE kHighlightAlphaLight = 26;

// GDI+ needs one-time process startup. Same function-local-static shape,
// and for the same reason, as AltTabHighlightBorder's own copy: this app
// is tray-resident for its whole lifetime and exits the process directly,
// so there is no meaningful moment to call GdiplusShutdown.
void EnsureGdiplusStarted() {
    static const struct GdiplusInit {
        ULONG_PTR token = 0;
        GdiplusInit() {
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&token, &input, nullptr);
        }
    } kInit;
    (void)kInit;
}

// The taskbar follows "Windows mode" (SystemUsesLightTheme), which is a
// different setting from the app theme every other window in this app
// asks polish::IsDarkModeEnabled() about (AppsUseLightTheme). The two can
// be set independently, and this paint lands on the taskbar.
bool TaskbarIsDark() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return true;  // dark is the default, and the safer guess on failure
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    const bool ok = RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type,
                                     reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    return ok ? value == 0 : true;
}

// Premultiplies B/G/R by A in place -- required before UpdateLayeredWindow
// with ULW_ALPHA, and not something GDI+ does when drawing into a buffer
// we own.
void PremultiplyAlpha(BYTE* pixels, int pixelCount) {
    for (int i = 0; i < pixelCount; ++i) {
        BYTE* p = pixels + i * 4;
        const BYTE a = p[3];
        p[0] = static_cast<BYTE>(p[0] * a / 255);
        p[1] = static_cast<BYTE>(p[1] * a / 255);
        p[2] = static_cast<BYTE>(p[2] * a / 255);
    }
}

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
    const float d = radius * 2;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

LRESULT CALLBACK ShieldProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) {
        // Claims the point, which is the whole feature: the taskbar never
        // sees the pointer here, so its flyout dwell never starts.
        //
        // Unconditional on purpose. This was once conditional -- answer
        // HTTRANSPARENT while a button was down, so clicks would fall
        // through to the taskbar -- and that does not work across a
        // process boundary at all (see SetPassThrough, which is what
        // replaced it). Nothing about the hit-test can distinguish a
        // hover from a click anyway: there is one verdict per dispatch
        // and it carries no idea what it is being asked for.
        //
        // DefWindowProcW would answer HTCLIENT for this window regardless
        // (a WS_POPUP with no frame is all client area). Spelled out so
        // the answer is a decision rather than a default.
        return HTCLIENT;
    }
    if (msg == WM_ERASEBKGND) {
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
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
    wc.lpfnWndProc = ShieldProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    // No cursor of its own: a shield that set one would change the
    // pointer's appearance over the taskbar, which is exactly the kind of
    // visible tell this window exists to avoid. With none, the cursor
    // keeps whatever it already was.
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

TaskbarShield::TaskbarShield(HINSTANCE instance) : instance_(instance) { EnsureClassRegistered(instance); }

TaskbarShield::~TaskbarShield() {
    for (const Shield& shield : shields_) {
        if (shield.window != nullptr) {
            DestroyWindow(shield.window);
        }
    }
}

HWND TaskbarShield::CreateShieldWindow() {
    // WS_EX_TOPMOST so it sits above Shell_TrayWnd (measured at z-depth 8
    // against the taskbar's 14 -- a plain topmost window is enough, which
    // is the same thing the bullseye already relies on).
    // WS_EX_NOACTIVATE so clicking near it never steals foreground.
    // WS_EX_TOOLWINDOW to keep it out of the native Alt+Tab list.
    // WS_EX_LAYERED for the alpha.
    //
    // Created absorbing -- WS_EX_TRANSPARENT is added and removed later,
    // per gesture, by SetPassThrough. A shield born with it would let the
    // very first hover through and show the flyout once.
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName,
                                  L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, nullptr);
    if (window != nullptr) {
        // No SetLayeredWindowAttributes: the surface is painted per-pixel
        // by Render, and the two ways of making a layered window
        // translucent are mutually exclusive.
        if (passThrough_) {
            // A taskbar that appeared mid-gesture (explorer restarting
            // under a held button) must match the shields that already
            // exist, or it would absorb an event its siblings pass on.
            SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        }
    }
    return window;
}

void TaskbarShield::SetPassThrough(bool passThrough) {
    if (passThrough == passThrough_) {
        return;
    }
    passThrough_ = passThrough;
    if (passThrough) {
        ++passThroughOpens_;
    }
    for (const Shield& shield : shields_) {
        if (shield.window == nullptr) {
            continue;
        }
        const LONG_PTR current = GetWindowLongPtrW(shield.window, GWL_EXSTYLE);
        const LONG_PTR updated = passThrough ? (current | WS_EX_TRANSPARENT) : (current & ~WS_EX_TRANSPARENT);
        // No SetWindowPos/SWP_FRAMECHANGED afterwards: WS_EX_TRANSPARENT
        // changes hit-testing, not the frame, and this runs from inside
        // the low-level mouse hook where anything that could pump
        // messages is out of the question.
        SetWindowLongPtrW(shield.window, GWL_EXSTYLE, updated);
    }
}

void TaskbarShield::Render(Shield& shield) {
    const int width = shield.rect.right - shield.rect.left;
    const int height = shield.rect.bottom - shield.rect.top;
    if (shield.window == nullptr || width <= 0 || height <= 0) {
        return;
    }
    RECT overlap{};
    const bool showHighlight =
        highlight_.has_value() && IntersectRect(&overlap, &shield.rect, &*highlight_) != FALSE;
    const RECT wanted = showHighlight ? *highlight_ : RECT{};
    if (shield.renderedSize.cx == width && shield.renderedSize.cy == height &&
        shield.renderedHighlight == showHighlight && EqualRect(&shield.renderedHighlightRect, &wanted)) {
        return;  // nothing about the surface would change
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // negative = top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        return;
    }

    // The whole surface at alpha 1: invisible, but still in the hit-test,
    // which is the shield's entire reason to exist.
    BYTE* pixels = static_cast<BYTE*>(bits);
    for (int i = 0; i < width * height; ++i) {
        pixels[i * 4 + 0] = 0;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = kShieldAlpha;
    }

    if (showHighlight) {
        EnsureGdiplusStarted();
        const UINT dpi = GetDpiForWindow(shield.window);
        const auto scale = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };
        const float insetX = static_cast<float>(scale(kHighlightInsetXDip));
        const float insetY = static_cast<float>(scale(kHighlightInsetYDip));
        const float radius = static_cast<float>(scale(kHighlightRadiusDip));
        const float x = static_cast<float>(wanted.left - shield.rect.left) + insetX;
        const float y = static_cast<float>(wanted.top - shield.rect.top) + insetY;
        const float w = static_cast<float>(wanted.right - wanted.left) - insetX * 2;
        const float h = static_cast<float>(wanted.bottom - wanted.top) - insetY * 2;
        if (w > 0 && h > 0) {
            const bool dark = TaskbarIsDark();
            const Gdiplus::Color fill(dark ? kHighlightAlphaDark : kHighlightAlphaLight, dark ? 255 : 0,
                                      dark ? 255 : 0, dark ? 255 : 0);
            Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppARGB, pixels);
            Gdiplus::Graphics graphics(&bitmap);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath path;
            AddRoundedRectPath(path, x, y, w, h, std::min(radius, std::min(w, h) / 2));
            Gdiplus::SolidBrush brush(fill);
            graphics.FillPath(&brush, &path);
        }
    }

    PremultiplyAlpha(pixels, width * height);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, dib));
    POINT source{0, 0};
    SIZE size{width, height};
    POINT destination{shield.rect.left, shield.rect.top};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(shield.window, screenDC, &destination, &size, memDC, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    DeleteObject(dib);

    shield.renderedSize = size;
    shield.renderedHighlight = showHighlight;
    shield.renderedHighlightRect = wanted;
}

void TaskbarShield::SetHighlight(std::optional<RECT> buttonRect) {
    if (highlight_.has_value() == buttonRect.has_value() &&
        (!highlight_.has_value() || EqualRect(&*highlight_, &*buttonRect))) {
        return;
    }
    highlight_ = buttonRect;
    for (Shield& shield : shields_) {
        Render(shield);
    }
}

void TaskbarShield::Update(const std::vector<TaskbarButton>& buttons) {
    // The union of each taskbar's own buttons. Taskbars are keyed by
    // HMONITOR rather than by index because AutomationId values repeat
    // across taskbars -- the same app pinned on two monitors gives two
    // buttons that are identical but for this.
    struct Strip {
        HMONITOR taskbar;
        RECT rect;
    };
    std::vector<Strip> strips;
    for (const TaskbarButton& button : buttons) {
        auto existing = std::find_if(strips.begin(), strips.end(),
                                     [&button](const Strip& strip) { return strip.taskbar == button.taskbar; });
        if (existing == strips.end()) {
            strips.push_back(Strip{button.taskbar, button.rect});
            continue;
        }
        UnionRect(&existing->rect, &existing->rect, &button.rect);
    }

    for (const Strip& strip : strips) {
        auto shield = std::find_if(shields_.begin(), shields_.end(),
                                   [&strip](const Shield& candidate) { return candidate.taskbar == strip.taskbar; });
        if (shield == shields_.end()) {
            shields_.push_back(Shield{strip.taskbar, CreateShieldWindow(), RECT{}});
            shield = shields_.end() - 1;
        }
        if (shield->window == nullptr) {
            continue;
        }
        // Re-asserting HWND_TOPMOST on every update, not only on a move:
        // another topmost window created since the last one would
        // otherwise sit above this and take the hover back. The rect
        // check only skips the move, never the z-order.
        const bool moved = !EqualRect(&shield->rect, &strip.rect);
        SetWindowPos(shield->window, HWND_TOPMOST, strip.rect.left, strip.rect.top, strip.rect.right - strip.rect.left,
                     strip.rect.bottom - strip.rect.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | (moved ? 0 : SWP_NOMOVE | SWP_NOSIZE));
        shield->rect = strip.rect;
        shield->missedUpdates = 0;
        // After the move, so the surface is painted at the size the
        // window now has. UpdateLayeredWindow also carries the position,
        // which is why this cannot run before SetWindowPos.
        Render(*shield);
    }

    // A taskbar that has gone away (monitor disconnected) or has no app
    // buttons left on it. Hidden rather than destroyed -- monitors come
    // back, and an empty taskbar fills up again the moment an app opens.
    //
    // Not on the first absence, though. A secondary taskbar that fails to
    // read contributes no buttons and says nothing (UiaWorker ignores its
    // result), so a single missing taskbar is as likely to be a flaky read
    // as a real change -- and hiding the shield over one that is still
    // there hands its buttons to the native flyout. Same reasoning, and the
    // same threshold, as TaskbarReadPolicy for the primary taskbar.
    for (Shield& shield : shields_) {
        const bool stillCovered = std::any_of(strips.begin(), strips.end(), [&shield](const Strip& strip) {
            return strip.taskbar == shield.taskbar;
        });
        if (stillCovered || shield.window == nullptr) {
            continue;
        }
        if (++shield.missedUpdates >= kBadReadsBeforeDegrade) {
            ShowWindow(shield.window, SW_HIDE);
            shield.rect = RECT{};
            shield.renderedSize = SIZE{};
        }
    }
}

bool TaskbarShield::CoversPoint(POINT screenPoint) const {
    const HWND at = WindowFromPoint(screenPoint);
    for (const Shield& shield : shields_) {
        if (shield.window != nullptr && shield.window == at) {
            return true;
        }
    }
    return false;
}

void TaskbarShield::Hide() {
    highlight_.reset();
    for (Shield& shield : shields_) {
        if (shield.window != nullptr) {
            ShowWindow(shield.window, SW_HIDE);
            shield.rect = RECT{};
            shield.renderedSize = SIZE{};
        }
    }
}

}  // namespace polish

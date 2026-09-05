#include "hook/AltTabHighlightBorder.h"

// GDI+ headers need IStream from <objidl.h>, which WIN32_LEAN_AND_MEAN
// (defined project-wide) otherwise excludes from <windows.h>.
#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishAltTabHighlightBorder";

// Windows accent blue -- matches resources/polish.ico's sparkle color.
// Gdiplus::Color's constructor isn't constexpr, so this is plain const.
const Gdiplus::Color kBorderColor(255, 0, 120, 212);

// GDI+ requires one-time process startup; this app is tray-resident for
// its whole lifetime and exits the process directly, so there's no
// meaningful moment to call GdiplusShutdown -- a function-local static
// (constructed once, on first use) is simpler and safer than trying to
// time a clean shutdown relative to some other object's destruction.
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

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // UpdateLayeredWindow (used exclusively for this window's content --
    // see ShowAroundTarget) bypasses WM_PAINT entirely; DefWindowProcW
    // is all that's needed here.
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

// Builds directly into `path` (out-param) rather than returning by
// value -- Gdiplus::GraphicsPath's copy constructor is protected.
// (offsetX, offsetY) shifts the whole shape -- used to build the inner
// cutout path inset from the outer one, without a separate Matrix
// transform.
void BuildRoundedRectPath(Gdiplus::GraphicsPath& path, int width, int height, int radius, int offsetX = 0,
                           int offsetY = 0) {
    radius = std::min(radius, std::min(width, height) / 2);
    const int d = radius * 2;
    const float x = static_cast<float>(offsetX);
    const float y = static_cast<float>(offsetY);
    path.AddArc(x, y, static_cast<float>(d), static_cast<float>(d), 180.0f, 90.0f);
    path.AddArc(x + static_cast<float>(width - d), y, static_cast<float>(d), static_cast<float>(d), 270.0f, 90.0f);
    path.AddArc(x + static_cast<float>(width - d), y + static_cast<float>(height - d), static_cast<float>(d),
                static_cast<float>(d), 0.0f, 90.0f);
    path.AddArc(x, y + static_cast<float>(height - d), static_cast<float>(d), static_cast<float>(d), 90.0f, 90.0f);
    path.CloseFigure();
}

// Premultiplies B/G/R by A in place -- required before UpdateLayeredWindow
// with ULW_ALPHA; GDI+ does not do this for us when drawing into an
// external buffer directly (see the class comment for why that's the
// only alpha-correct way to draw into it at all).
void PremultiplyAlpha(BYTE* pixels, int pixelCount) {
    for (int i = 0; i < pixelCount; ++i) {
        BYTE* p = pixels + i * 4;
        const BYTE a = p[3];
        p[0] = static_cast<BYTE>(p[0] * a / 255);
        p[1] = static_cast<BYTE>(p[1] * a / 255);
        p[2] = static_cast<BYTE>(p[2] * a / 255);
    }
}

}  // namespace

AltTabHighlightBorder::AltTabHighlightBorder(HINSTANCE instance) {
    EnsureGdiplusStarted();
    EnsureClassRegistered(instance);
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                               kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
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
    // The ring's actual visible width -- unlike the old fading-gradient
    // version, this is now a real, solid dimension, not a fade zone.
    const int thickness = MulDiv(3, static_cast<int>(dpi), 96);
    // Matches Windows 11's own default window-corner rounding (~8px at
    // 96 DPI -- what VS Code and most native apps use), not a bigger,
    // more obviously-rounded shape of its own.
    const int radius = MulDiv(8, static_cast<int>(dpi), 96);

    // On the window's own rect, not inflated outward -- the ring sits
    // inside the target's own edge, into its own content, rather than
    // projecting out into the desktop margin around it. Everywhere more
    // than `thickness` in from the edge, the target's real content shows
    // through completely untouched.
    const RECT outer = targetRect;
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    // Ensure topmost + visible without touching position/size here --
    // that happens atomically together with the content update, in the
    // single UpdateLayeredWindow call below. Doing position/size via a
    // separate SetWindowPos first (as an earlier version of this
    // function did) left a visible gap between "window moved/resized to
    // the new target" and "content repainted for the new size", showing
    // up as the previous target's rendered content briefly stretched
    // into the new window bounds before catching up.
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

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

    {
        // External-buffer Bitmap constructor, not Graphics(HDC) / a
        // Graphics wrapping some other HDC -- the latter falls back to
        // GDI-compatibility rendering and does not reliably write alpha
        // into the backing DIB (the exact bug PLAN.md's history already
        // documents from this app's earlier UI work).
        Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::GraphicsPath path;
        BuildRoundedRectPath(path, width, height, radius);

        // Clamp so a target smaller than 2x the ring thickness never gets
        // fully painted over -- some interior always stays uncovered,
        // however small the target. (With the old fading gradient this
        // clamp wasn't needed: the fill was already transparent well
        // before reaching the interior on any realistically-sized window,
        // so painting the full path there was harmless. With a flat
        // opaque fill it no longer is.)
        const int effectiveThickness = std::min({thickness, width / 2, height / 2});

        // Carve the hollow ring by excluding an inner rounded-rect path
        // inset by the ring's own thickness -- not a plain axis-aligned
        // rect, which would show as a wrong-shaped inner corner once the
        // fill is fully opaque (harmless with the old gradient, since it
        // had already faded to near-zero well before reaching the
        // interior either way). This is also still a real perf win, same
        // as before: GDI+ only rasterizes pixels inside the clip, so cost
        // stays roughly constant regardless of target window size, which
        // is what fixed a human-reported "flash" glitch (up to 125ms for
        // a large/maximized target) once already in this class's history.
        Gdiplus::GraphicsPath innerPath;
        BuildRoundedRectPath(innerPath, width - 2 * effectiveThickness, height - 2 * effectiveThickness,
                              std::max(0, radius - effectiveThickness), effectiveThickness, effectiveThickness);
        Gdiplus::Region clipRegion(&path);
        clipRegion.Exclude(&innerPath);
        graphics.SetClip(&clipRegion);

        Gdiplus::SolidBrush brush(kBorderColor);
        graphics.FillPath(&brush, &path);
    }

    PremultiplyAlpha(static_cast<BYTE*>(bits), width * height);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, dib));

    POINT srcPoint{0, 0};
    POINT dstPoint{outer.left, outer.top};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screenDC, &dstPoint, &size, memDC, &srcPoint, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    DeleteObject(dib);
}

void AltTabHighlightBorder::Hide() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
}

}  // namespace polish

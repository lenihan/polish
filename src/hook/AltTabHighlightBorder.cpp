#include "hook/AltTabHighlightBorder.h"

#include <dwmapi.h>

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

// Independent per-corner radii -- needed because a maximized/fullscreen
// target's corners aren't all equal: whichever corners sit flush against
// the monitor's own physical edges need to match the *screen's* hardware
// corner rounding (some devices, e.g. certain Surface models, physically
// round the display panel's corners), while any corner inset from the
// screen edge (e.g. by the taskbar) still wants the normal small
// window-corner radius. See ShowAroundTarget for how these are chosen.
struct CornerRadii {
    int topLeft;
    int topRight;
    int bottomRight;
    int bottomLeft;
};

CornerRadii UniformRadii(int radius) { return {radius, radius, radius, radius}; }

// Builds directly into `path` (out-param) rather than returning by
// value -- Gdiplus::GraphicsPath's copy constructor is protected.
// (offsetX, offsetY) shifts the whole shape -- used to build the inner
// cutout path inset from the outer one, without a separate Matrix
// transform.
void BuildRoundedRectPath(Gdiplus::GraphicsPath& path, int width, int height, CornerRadii radii, int offsetX = 0,
                           int offsetY = 0) {
    const int maxRadius = std::min(width, height) / 2;
    const int topLeft = std::min(radii.topLeft, maxRadius);
    const int topRight = std::min(radii.topRight, maxRadius);
    const int bottomRight = std::min(radii.bottomRight, maxRadius);
    const int bottomLeft = std::min(radii.bottomLeft, maxRadius);
    const float x = static_cast<float>(offsetX);
    const float y = static_cast<float>(offsetY);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float dTL = static_cast<float>(topLeft * 2);
    const float dTR = static_cast<float>(topRight * 2);
    const float dBR = static_cast<float>(bottomRight * 2);
    const float dBL = static_cast<float>(bottomLeft * 2);
    path.AddArc(x, y, dTL, dTL, 180.0f, 90.0f);
    path.AddArc(x + w - dTR, y, dTR, dTR, 270.0f, 90.0f);
    path.AddArc(x + w - dBR, y + h - dBR, dBR, dBR, 0.0f, 90.0f);
    path.AddArc(x, y + h - dBL, dBL, dBL, 90.0f, 90.0f);
    path.CloseFigure();
}

// Premultiplies B/G/R by A in place -- required before UpdateLayeredWindow
// with ULW_ALPHA; GDI+ does not do this for us when drawing into an
// external buffer directly (see the class comment for why that's the
// only alpha-correct way to draw into it at all).
// GetWindowRect includes the modern invisible resize border (present even
// on a window with no visible frame there), which Windows deliberately
// hangs a few px *off* the monitor's edges for a maximized window so the
// window's actually-visible edge lines up exactly with the screen edge.
// Drawing a thin ring on the raw GetWindowRect rect of a maximized window
// therefore lands most or all of it off-screen -- confirmed as a real,
// human-reported bug (invisible ring on a maximized/full-screen app).
// DWMWA_EXTENDED_FRAME_BOUNDS gives the tighter, actually-visible rect
// instead (excludes that invisible border), which is what this class
// wants to draw *on*. This is a single fresh query for a one-off visual
// rect, not a stored/round-tripped value mixed with GetWindowRect from
// another code path -- unrelated to the restore-position-sync gotcha
// elsewhere in this app about not mixing the two across a round trip.
bool GetVisibleWindowRect(HWND hwnd, RECT& rect) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        return true;
    }
    return GetWindowRect(hwnd, &rect) != FALSE;
}

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
    if (!GetVisibleWindowRect(target, targetRect)) {
        return;
    }

    const UINT dpi = GetDpiForWindow(target);
    // The ring's actual visible width -- unlike the old fading-gradient
    // version, this is now a real, solid dimension, not a fade zone.
    const int thickness = MulDiv(3, static_cast<int>(dpi), 96);
    // Matches Windows 11's own default window-corner rounding (~8px at
    // 96 DPI -- what VS Code and most native apps use), not a bigger,
    // more obviously-rounded shape of its own.
    const int windowRadius = MulDiv(8, static_cast<int>(dpi), 96);
    // Some devices (certain Surface models) physically round the display
    // panel's own corners -- confirmed real, human-reported: a thin ring
    // drawn with the normal window-corner radius visibly got chopped off
    // right at the screen's four corners on a maximized/fullscreen target,
    // because the hardware rounding there is a good deal more aggressive
    // than a typical app window's own corner radius. Not queryable via any
    // documented API (this is a hardware/compositor-level effect, not a
    // per-window style), so this is a tuned constant -- like every other
    // thickness/radius value in this class, confirm/adjust after seeing it
    // live rather than trusting the number in isolation.
    const int screenRadius = MulDiv(28, static_cast<int>(dpi), 96);

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

    // Per-corner: use the screen's own (larger) corner radius only for a
    // corner that's actually flush against the monitor's physical edge on
    // both sides -- e.g. a normal maximized window (taskbar at the
    // bottom) has its top-left/top-right corners flush against the
    // screen, but its bottom two corners inset by the taskbar, so only
    // the top two need the screen radius. A small tolerance (a couple px)
    // absorbs any minor rounding between DWM's extended-frame-bounds and
    // the monitor rect. Every corner falls back to the normal window
    // radius (including on a monitor lookup failure) -- exactly today's
    // existing behavior for any window that isn't flush against a screen
    // edge at all.
    CornerRadii radii = UniformRadii(windowRadius);
    if (HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            constexpr int kEdgeTolerance = 2;
            const RECT& screen = monitorInfo.rcMonitor;
            const bool flushLeft = std::abs(outer.left - screen.left) <= kEdgeTolerance;
            const bool flushTop = std::abs(outer.top - screen.top) <= kEdgeTolerance;
            const bool flushRight = std::abs(outer.right - screen.right) <= kEdgeTolerance;
            const bool flushBottom = std::abs(outer.bottom - screen.bottom) <= kEdgeTolerance;
            if (flushLeft && flushTop) {
                radii.topLeft = screenRadius;
            }
            if (flushRight && flushTop) {
                radii.topRight = screenRadius;
            }
            if (flushRight && flushBottom) {
                radii.bottomRight = screenRadius;
            }
            if (flushLeft && flushBottom) {
                radii.bottomLeft = screenRadius;
            }
        }
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
        BuildRoundedRectPath(path, width, height, radii);

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
        // Each corner's inner radius shrinks from that same corner's outer
        // radius, so a big screen-matched corner still fades down to a
        // correctly-shaped (not axis-aligned) inner edge.
        const CornerRadii innerRadii{
            std::max(0, radii.topLeft - effectiveThickness),
            std::max(0, radii.topRight - effectiveThickness),
            std::max(0, radii.bottomRight - effectiveThickness),
            std::max(0, radii.bottomLeft - effectiveThickness),
        };
        Gdiplus::GraphicsPath innerPath;
        BuildRoundedRectPath(innerPath, width - 2 * effectiveThickness, height - 2 * effectiveThickness, innerRadii,
                              effectiveThickness, effectiveThickness);
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

#include "hook/ActiveWindowHalo.h"

#include <shellscalingapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

#include "util/DarkMode.h"
#include "util/Logging.h"
#include "windowtracking/RectUtils.h"

namespace polish {

namespace halo_math {

float RoundedRectDistance(float px, float py, float halfWidth, float halfHeight, float radius) {
    const float r = std::min({radius, halfWidth, halfHeight});
    const float qx = std::fabs(px) - (halfWidth - r);
    const float qy = std::fabs(py) - (halfHeight - r);
    const float maxQx = std::max(qx, 0.0f);
    const float maxQy = std::max(qy, 0.0f);
    const float outside = std::sqrt(maxQx * maxQx + maxQy * maxQy);
    const float inside = std::min(std::max(qx, qy), 0.0f);
    return outside + inside - r;
}

int DistanceToAlpha(float d, int halo, int peak) {
    if (halo <= 0) {
        return d <= 0.0f ? peak : 0;
    }
    const float t = std::clamp(d / static_cast<float>(halo), 0.0f, 1.0f);
    const float falloff = (1.0f - t) * (1.0f - t);
    return static_cast<int>(static_cast<float>(peak) * falloff + 0.5f);
}

}  // namespace halo_math

namespace {

// Pins the halo directly beneath `target` in the Z-order, which is the
// only placement that actually holds.
//
// HWND_TOP does not work here, and returns success while doing nothing:
// Windows refuses to let a background process put a non-topmost window
// above the *foreground* window, and the foreground window is exactly
// what this overlay always hugs. Confirmed live -- the halo would stall
// at a fixed Z index several slots behind its own target and get buried
// under whatever else was on screen (a full-screen editor, typically),
// which read as "the halo just isn't there".
//
// Directly beneath the target is also better than above it: the target
// covers only the glow's own middle, which it would cover anyway, and
// nothing that the target is in front of can ever come between the two.
// HWND_TOP stays as the fallback for a target that has since died.
bool PlaceHaloBehindTarget(HWND halo, HWND target, UINT extraFlags) {
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | extraFlags;
    if (target != nullptr && IsWindow(target)) {
        return SetWindowPos(halo, target, 0, 0, 0, 0, flags) != FALSE;
    }
    return SetWindowPos(halo, HWND_TOP, 0, 0, 0, 0, flags) != FALSE;
}

}  // namespace

namespace {

constexpr wchar_t kClassName[] = L"PolishActiveWindowHalo";

// Same tolerance/meaning as AltTabHighlightBorder's own flush-edge check
// (AltTabHighlightBorder.cpp) -- absorbs any minor rounding between DWM's
// extended-frame-bounds and the monitor rect.
constexpr int kEdgeTolerance = 2;

// Peak alpha at the target's own edge, tuned by eye -- separate per theme
// because the two don't read the same at equal alpha: a black glow this
// strong reads as a heavy drop shadow around every window, unpleasant at
// the white value's 190. Adjust after seeing both live (see PLAN.md's
// verification walkthrough).
constexpr int kPeakAlphaDark = 190;
constexpr int kPeakAlphaLight = 100;

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // UpdateLayeredWindow (used exclusively for this window's content)
    // bypasses WM_PAINT entirely -- DefWindowProcW is all that's needed,
    // same reasoning as AltTabHighlightBorder's own EnsureClassRegistered.
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

// Because the color is always pure white or pure black, premultiplying is
// a single store per pixel rather than a separate per-pixel multiply pass:
// premultiplied white is (a,a,a,a) in every channel, which as a top-down
// 32bpp BGRA store (alpha in the high byte) is just alpha broadcast into
// every byte; premultiplied black is (0,0,0,a), alpha alone in the high
// byte.
uint32_t PremultipliedPixel(int alpha, bool white) {
    const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0, 255));
    return white ? a * 0x01010101u : (a << 24);
}

}  // namespace

ActiveWindowHalo::ActiveWindowHalo(HINSTANCE instance) {
    EnsureClassRegistered(instance);
    // WS_EX_TOOLWINDOW is load-bearing here -- unlike AltTabHighlightBorder,
    // which doesn't need it (session-scoped, never sits around long enough
    // to matter): without it, this permanently-visible top-level window
    // shows up in Task View. No `owner` -- a cross-process owner
    // relationship attaches input queues and would destroy the halo along
    // with whatever real window happened to be its target at the time.
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    window_ = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    // COM STA is already initialized by the time this is constructed --
    // see wWinMain. A failure here just leaves virtualDesktopManager_
    // nullptr, and FollowTargetVirtualDesktop degrades to a silent no-op
    // (no desktop-following, but the halo otherwise still works).
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                      IID_PPV_ARGS(&virtualDesktopManager_));
}

ActiveWindowHalo::~ActiveWindowHalo() {
    if (window_ != nullptr && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
    }
    if (virtualDesktopManager_ != nullptr) {
        virtualDesktopManager_->Release();
    }
}

void ActiveWindowHalo::Hide() {
    if (window_ != nullptr && visible_) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
    // Not a real target -- forces the next ShowAroundTarget to re-assert
    // Z-order/SWP_SHOWWINDOW even if it happens to be shown for the exact
    // same target as before hiding.
    lastTarget_ = nullptr;
}

void ActiveWindowHalo::FollowTargetVirtualDesktop(HWND target) {
    if (virtualDesktopManager_ == nullptr) {
        return;
    }
    GUID desktopId{};
    if (FAILED(virtualDesktopManager_->GetWindowDesktopId(target, &desktopId))) {
        return;
    }
    if (IsEqualGUID(desktopId, lastKnownDesktopId_)) {
        return;
    }
    wchar_t guidText[64]{};
    StringFromGUID2(desktopId, guidText, static_cast<int>(std::size(guidText)));
    polish::LogDebug(std::format(L"[Polish] Halo: target's virtual desktop id changed to {}", guidText));
    lastKnownDesktopId_ = desktopId;
    // Deliberately not calling MoveWindowToDesktop(window_, desktopId)
    // yet -- PLAN.md flags this as needing live confirmation first (does
    // this window actually stop appearing across a desktop switch the way
    // a persistent top-level window created once at startup theoretically
    // should?) before adding the fix. Logging every change here is enough
    // to answer that from %TEMP%\polish.log; add the MoveWindowToDesktop
    // call if the live walkthrough confirms it's a real problem.
}

void ActiveWindowHalo::ShowAroundTarget(HWND target) {
    if (window_ == nullptr) {
        return;
    }

    RECT targetRect;
    if (!GetVisibleWindowRect(target, targetRect)) {
        Hide();
        return;
    }
    const int targetWidth = targetRect.right - targetRect.left;
    const int targetHeight = targetRect.bottom - targetRect.top;
    if (targetWidth <= 0 || targetHeight <= 0) {
        // Nothing sane to draw around (e.g. target mid-minimize) -- hide
        // rather than leaving stale content on screen at a stale position,
        // matching AltTabHighlightBorder's own guard for the same case.
        Hide();
        return;
    }

    FollowTargetVirtualDesktop(target);

    // DPI comes from the monitor, not the window: GetDpiForWindow(target)
    // returns the *target's own* DPI-awareness context, so a system-DPI-
    // aware or DPI-unaware app sitting on a high-DPI monitor would report
    // 96 here and get a half-size halo. The halo is our own visual,
    // painted in raw screen pixels, so it needs the monitor's actual DPI
    // regardless of what target itself thinks its DPI is.
    HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const bool haveMonitorInfo = monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo);
    if (monitor != nullptr) {
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    }
    const UINT dpi = dpiX;

    // A quarter inch at 96 DPI.
    const int halo = MulDiv(24, static_cast<int>(dpi), 96);

    // Per-side inflation: zero on any side within kEdgeTolerance of its
    // monitor's own edge. Without this, a window snapped to the right
    // half of monitor 1 would paint a glow strip down the left edge of
    // monitor 2, floating next to nothing it's actually attached to.
    RECT inflate{halo, halo, halo, halo};
    if (haveMonitorInfo) {
        const RECT& screen = monitorInfo.rcMonitor;
        if (std::abs(targetRect.left - screen.left) <= kEdgeTolerance) {
            inflate.left = 0;
        }
        if (std::abs(targetRect.top - screen.top) <= kEdgeTolerance) {
            inflate.top = 0;
        }
        if (std::abs(targetRect.right - screen.right) <= kEdgeTolerance) {
            inflate.right = 0;
        }
        if (std::abs(targetRect.bottom - screen.bottom) <= kEdgeTolerance) {
            inflate.bottom = 0;
        }
    }

    const bool isDark = IsDarkModeEnabled();
    const int glowWidth = targetWidth + inflate.left + inflate.right;
    const int glowHeight = targetHeight + inflate.top + inflate.bottom;

    const bool targetChanged = (target != lastTarget_);
    lastTarget_ = target;

    POINT dstPoint{targetRect.left - inflate.left, targetRect.top - inflate.top};

    // Cache key: (glow width, glow height, dpi, isDark). Glow width/height
    // (not just the target's own size) already bakes in the per-side
    // inflation, so a target that crosses a monitor's flush edge without
    // changing its own size still correctly triggers a full re-render
    // here, even though CachedTargetSize() (target-size-only, for callers
    // debouncing resize bursts) would have reported "unchanged".
    const bool sameContent =
        (glowWidth == width_ && glowHeight == height_ && dpi == dpi_ && isDark == isDark_ && bits_ != nullptr);

    if (sameContent) {
        // Cheap move-only path: reposition without touching content
        // (documented UpdateLayeredWindow behavior when hdcSrc is null),
        // then just assert Z-order/visibility if either actually needs to
        // change -- SWP_NOZORDER otherwise, since nothing is changing
        // Z-order 60x/sec during a drag.
        UpdateLayeredWindow(window_, nullptr, &dstPoint, nullptr, nullptr, nullptr, 0, nullptr, 0);
        if (!visible_ || targetChanged) {
            PlaceHaloBehindTarget(window_, target, SWP_SHOWWINDOW);
            visible_ = true;
        } else {
            SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return;
    }

    const ULONGLONG renderStartMs = GetTickCount64();
    Render(targetWidth, targetHeight, inflate, dpi, isDark);
    cachedTargetSize_ = SIZE{targetWidth, targetHeight};
    const ULONGLONG renderDurationMs = GetTickCount64() - renderStartMs;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, dib_));

    POINT srcPoint{0, 0};
    SIZE size{glowWidth, glowHeight};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screenDC, &dstPoint, &size, memDC, &srcPoint, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    // Order matters, and differs from AltTabHighlightBorder: UpdateLayeredWindow
    // above already set position, size and content atomically, so
    // SetWindowPos here only needs to (re)assert Z-order/visibility, after
    // content is already correct -- showing first (as AltTabHighlightBorder
    // does, harmlessly, since it's session-scoped) would flash the previous
    // frame at the previous position for a persistent overlay that
    // repeatedly re-shows like this one.
    PlaceHaloBehindTarget(window_, target, SWP_SHOWWINDOW);
    visible_ = true;

    polish::LogDebug(std::format(
        L"[Polish] Halo: render target={} rect=({},{})-({},{}) dpi={} dark={} durationMs={}",
        reinterpret_cast<void*>(target), targetRect.left, targetRect.top, targetRect.right, targetRect.bottom, dpi,
        isDark, renderDurationMs));
}

void ActiveWindowHalo::EnsureDibCapacity(int width, int height) {
    if (width <= capWidth_ && height <= capHeight_) {
        return;
    }
    const int newCapWidth = std::max(width, capWidth_);
    const int newCapHeight = std::max(height, capHeight_);

    if (dib_ != nullptr) {
        DeleteObject(dib_);
        dib_ = nullptr;
        bits_ = nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = newCapWidth;
    bmi.bmiHeader.biHeight = -newCapHeight;  // negative = top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bits_ = static_cast<uint32_t*>(bits);
    capWidth_ = newCapWidth;
    capHeight_ = newCapHeight;

    // A fresh CreateDIBSection's pixels aren't part of any previously-
    // tracked band rect (and freshly-committed DIB memory comes back
    // zeroed in practice) -- nothing stale to clear before the render
    // that's about to fill this new capacity in from scratch.
    hasPreviousBands_ = false;
}

void ActiveWindowHalo::ClearRect(const RECT& rect) {
    if (bits_ == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    for (int y = rect.top; y < rect.bottom; ++y) {
        std::fill_n(bits_ + static_cast<size_t>(y) * capWidth_ + rect.left, rect.right - rect.left, 0u);
    }
}

void ActiveWindowHalo::Render(int targetWidth, int targetHeight, RECT inflate, UINT dpi, bool isDark) {
    const int glowWidth = targetWidth + inflate.left + inflate.right;
    const int glowHeight = targetHeight + inflate.top + inflate.bottom;
    EnsureDibCapacity(glowWidth, glowHeight);
    if (bits_ == nullptr) {
        return;
    }

    if (hasPreviousBands_) {
        ClearRect(prevTopBand_);
        ClearRect(prevBottomBand_);
        ClearRect(prevLeftBand_);
        ClearRect(prevRightBand_);
    }

    // Local coordinates: (0,0) is the glow window's own top-left, which is
    // inflate.left/inflate.top pixels up-and-left of the target's own
    // top-left. Staying in this fixed local frame (rather than screen
    // coordinates, which move every time the target does) is what makes
    // "clear last frame's bands before drawing this frame's" correct
    // regardless of where on screen the window itself currently sits.
    const RECT tl{inflate.left, inflate.top, inflate.left + targetWidth, inflate.top + targetHeight};

    const float halfWidth = targetWidth / 2.0f;
    const float halfHeight = targetHeight / 2.0f;
    const float centerX = static_cast<float>(tl.left) + halfWidth;
    const float centerY = static_cast<float>(tl.top) + halfHeight;

    // Matches Windows 11's own default window-corner rounding at 96 DPI --
    // same constant AltTabHighlightBorder's windowRadius uses, and its own
    // comment on where 8 came from. Clamped the same way
    // BuildRoundedRectPath already does, so a target smaller than 2x the
    // radius never produces an over-large corner.
    const int rInt = std::min(MulDiv(8, static_cast<int>(dpi), 96), std::min(targetWidth, targetHeight) / 2);
    const float r = static_cast<float>(std::max(rInt, 0));

    const int halo = std::max({inflate.left, inflate.top, inflate.right, inflate.bottom});
    const int peak = isDark ? kPeakAlphaDark : kPeakAlphaLight;

    const auto pixelPtr = [this](int x, int y) { return bits_ + static_cast<size_t>(y) * capWidth_ + x; };

    // The base peak-at-d=0 falloff (halo_math::DistanceToAlpha) plus two
    // adjustments that exist purely to smooth the rasterization, not to
    // shape the glow: a half-pixel blend at the inner (d ~ 0) boundary
    // (the outer boundary at d = halo is already smooth on its own, since
    // the base falloff itself reaches 0 there), and an explicit cutoff
    // below d = -0.5 so the interior -- which this renderer otherwise
    // never even visits -- would read as fully transparent if it ever did.
    const auto colorForDistance = [&](float d) -> uint32_t {
        if (d >= static_cast<float>(halo) || d < -0.5f) {
            return 0;
        }
        const int baseAlpha = halo_math::DistanceToAlpha(d, halo, peak);
        const float innerAntialias = std::clamp(d + 0.5f, 0.0f, 1.0f);
        const int alpha = static_cast<int>(static_cast<float>(baseAlpha) * innerAntialias + 0.5f);
        return PremultipliedPixel(alpha, isDark);
    };

    const int midLeft = tl.left + rInt;
    const int midRight = tl.right - rInt;
    const int midTop = tl.top + rInt;
    const int midBottom = tl.bottom - rInt;

    // Top/bottom bands, across the flat horizontal middle (x in
    // [midLeft, midRight)): away from the rounded corners, distance to the
    // target reduces to the 1D distance to its nearest straight edge --
    // one value per row, written with fill_n rather than recomputed per
    // pixel.
    if (midRight > midLeft) {
        for (int y = 0; y < inflate.top; ++y) {
            const float d = std::fabs(static_cast<float>(y) - centerY) - halfHeight;
            std::fill_n(pixelPtr(midLeft, y), midRight - midLeft, colorForDistance(d));
        }
        for (int y = tl.bottom; y < glowHeight; ++y) {
            const float d = std::fabs(static_cast<float>(y) - centerY) - halfHeight;
            std::fill_n(pixelPtr(midLeft, y), midRight - midLeft, colorForDistance(d));
        }
    }

    // Left/right bands, across the flat vertical middle (y in [midTop,
    // midBottom)): same reduction along the other axis -- one value per
    // column, computed once into a small row buffer and replicated down
    // every row in range via memcpy rather than recomputed per row.
    if (midBottom > midTop) {
        if (inflate.left > 0) {
            std::vector<uint32_t> row(static_cast<size_t>(inflate.left));
            for (int x = 0; x < inflate.left; ++x) {
                const float d = std::fabs(static_cast<float>(x) - centerX) - halfWidth;
                row[static_cast<size_t>(x)] = colorForDistance(d);
            }
            for (int y = midTop; y < midBottom; ++y) {
                std::memcpy(pixelPtr(0, y), row.data(), row.size() * sizeof(uint32_t));
            }
        }
        if (inflate.right > 0) {
            std::vector<uint32_t> row(static_cast<size_t>(inflate.right));
            for (int x = 0; x < inflate.right; ++x) {
                const float d = std::fabs(static_cast<float>(tl.right + x) - centerX) - halfWidth;
                row[static_cast<size_t>(x)] = colorForDistance(d);
            }
            for (int y = midTop; y < midBottom; ++y) {
                std::memcpy(pixelPtr(tl.right, y), row.data(), row.size() * sizeof(uint32_t));
            }
        }
    }

    // The four corner boxes -- roughly (halo + r)^2 pixels each, versus
    // ~(glowWidth*inflate.top + glowHeight*inflate.left) for the bands
    // above on a large window -- are the only pixels that need the real
    // 2D (sqrtf-based) distance, since they're where the rounded corner
    // actually curves. Each box's own bounds naturally collapse to empty
    // on a flush side (inflate 0 on that side puts tl's own edge right at
    // the window's own edge), so no separate flush-side special-casing is
    // needed here -- the loop bounds already do it.
    const auto renderCornerBox = [&](int xStart, int xEnd, int yStart, int yEnd) {
        for (int y = yStart; y < yEnd; ++y) {
            const float py = static_cast<float>(y) - centerY;
            for (int x = xStart; x < xEnd; ++x) {
                const float px = static_cast<float>(x) - centerX;
                const float d = halo_math::RoundedRectDistance(px, py, halfWidth, halfHeight, r);
                *pixelPtr(x, y) = colorForDistance(d);
            }
        }
    };
    renderCornerBox(0, midLeft, 0, midTop);                       // top-left
    renderCornerBox(midRight, glowWidth, 0, midTop);               // top-right
    renderCornerBox(0, midLeft, midBottom, glowHeight);            // bottom-left
    renderCornerBox(midRight, glowWidth, midBottom, glowHeight);   // bottom-right

    width_ = glowWidth;
    height_ = glowHeight;
    dpi_ = dpi;
    isDark_ = isDark;

    prevTopBand_ = RECT{0, 0, glowWidth, inflate.top};
    prevBottomBand_ = RECT{0, tl.bottom, glowWidth, glowHeight};
    prevLeftBand_ = RECT{0, inflate.top, inflate.left, tl.bottom};
    prevRightBand_ = RECT{tl.right, inflate.top, glowWidth, tl.bottom};
    hasPreviousBands_ = true;
}

}  // namespace polish

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

int OutlineAlpha(float d, int thickness, int alpha) {
    if (thickness <= 0 || d < 0.0f || d >= static_cast<float>(thickness)) {
        return 0;
    }
    // Antialiased over the last pixel of the outer edge only. The inner
    // edge needs none: it abuts the target, which covers everything
    // behind it.
    const float coverage = std::clamp(static_cast<float>(thickness) - d, 0.0f, 1.0f);
    return static_cast<int>(static_cast<float>(alpha) * coverage + 0.5f);
}

int Luminance(uint32_t colorref) {
    const uint32_t r = colorref & 0xFFu;
    const uint32_t g = (colorref >> 8) & 0xFFu;
    const uint32_t b = (colorref >> 16) & 0xFFu;
    return static_cast<int>((299u * r + 587u * g + 114u * b) / 1000u);
}

uint32_t ClampGlowLuminance(uint32_t colorref, int minLum, int maxLum) {
    if (minLum > maxLum) {
        return colorref;
    }
    const int lum = Luminance(colorref);
    if (lum >= minLum && lum <= maxLum) {
        return colorref;
    }
    if (lum <= 0) {
        // Pure black has no hue to preserve; scaling it stays black
        // however hard it is scaled.
        const uint32_t grey = static_cast<uint32_t>(std::clamp(minLum, 0, 255));
        return grey | (grey << 8) | (grey << 16);
    }
    const int targetLum = lum < minLum ? minLum : maxLum;
    const auto scale = [&](uint32_t channel) {
        return static_cast<uint32_t>(
            std::clamp(static_cast<int>(channel) * targetLum / lum, 0, 255));
    };
    return scale(colorref & 0xFFu) | (scale((colorref >> 8) & 0xFFu) << 8) |
           (scale((colorref >> 16) & 0xFFu) << 16);
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
//
// An elevated target cannot be referenced at all, which is the second
// fallback. UIPI refuses a z-order reference from this process to a
// window at a higher integrity level -- measured against Task Manager,
// where SetWindowPos returns FALSE with GetLastError 5
// (ERROR_ACCESS_DENIED). The halo was then left wherever it happened to
// be, which is to say underneath the windows the target is covering:
// reported as "I alt-tab to it and the halo is behind other windows".
//
// Topmost is the only placement left that holds. HWND_TOP does not
// work, for the reason above, and there is no way to raise an elevated
// window ourselves either. The cost is that the glow is then above the
// target rather than beneath it, so the couple of pixels it deliberately
// underlaps (kUnderlapDip) paint over that window's own border instead
// of behind it. Only elevated targets pay it -- and it is much the
// smaller of the two evils next to no halo at all.
enum class HaloPlacement { BehindTarget, AboveEverything, Failed };

HaloPlacement PlaceHaloBehindTarget(HWND halo, HWND target, UINT extraFlags) {
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | extraFlags;
    if (target == nullptr || !IsWindow(target)) {
        return SetWindowPos(halo, HWND_TOP, 0, 0, 0, 0, flags) != FALSE ? HaloPlacement::BehindTarget
                                                                        : HaloPlacement::Failed;
    }
    if (SetWindowPos(halo, target, 0, 0, 0, 0, flags) != FALSE) {
        // Also the path back from topmost: a non-topmost hWndInsertAfter
        // clears WS_EX_TOPMOST on the window being moved (documented), so
        // switching from an elevated window to an ordinary one puts the
        // halo back in the normal band without a separate call.
        return HaloPlacement::BehindTarget;
    }
    return SetWindowPos(halo, HWND_TOPMOST, 0, 0, 0, 0, flags) != FALSE ? HaloPlacement::AboveEverything
                                                                        : HaloPlacement::Failed;
}

}  // namespace

namespace {

constexpr wchar_t kClassName[] = L"PolishActiveWindowHalo";

// Same tolerance/meaning as AltTabHighlightBorder's own flush-edge check
// (AltTabHighlightBorder.cpp) -- absorbs any minor rounding between DWM's
// extended-frame-bounds and the monitor rect.
constexpr int kEdgeTolerance = 2;

// Peak alpha at the target's own edge, tuned by eye.
//
// One value, not one per theme. The old split existed because a *black*
// glow at the white value's 190 read as a heavy drop shadow around every
// window; the glow is no longer black in either theme, so the reason is
// gone with it.
constexpr int kPeakAlpha = 190;

// The band the accent colour's luminance is pulled into before it is
// used, so an accent that is nearly white or nearly black cannot put the
// glow back into the invisible-against-one-extreme state this whole
// approach exists to escape. Tuned by eye, like everything else here.
constexpr int kGlowMinLuminance = 60;
constexpr int kGlowMaxLuminance = 185;

// How far the glow keeps painting, at full peak alpha, *inside* the
// target's own edge (at 96 DPI) rather than stopping dead at d = 0.
//
// A Windows 11 window's outermost pixels aren't opaque: the thin frame
// border DWM draws around one is partly transparent, so whatever sits
// behind the window shows through it. The halo sits directly behind its
// target (see PlaceHaloBehindTarget), so that border ends up blended
// against the glow -- and an earlier version, which stopped painting at
// the target's own edge (and additionally *halved* alpha in the d = 0
// column, to antialias a seam that's covered anyway), left the border
// blending against a column that was part glow and part raw desktop.
// That read as a faint, uneven line hugging the window's edge, its
// brightness wobbling with whatever wallpaper happened to be behind it
// -- reported as the edge not looking perfectly straight, and
// reproducible only with the halo on.
//
// Painting a couple of pixels further in, all at peak alpha, gives that
// semi-transparent border a *uniform* backdrop the whole way along each
// edge, so it reads as the straight line it is. The extra pixels are
// free: they're always behind the target, so nothing but the target's
// own border ever samples them. They also absorb the case where the
// visible edge lands a pixel inside DWMWA_EXTENDED_FRAME_BOUNDS (what
// GetVisibleWindowRect reports), which is the other way this seam opens
// up.
constexpr int kUnderlapDip = 2;

// The contrasting outline ring: thickness in DIPs, and its alpha. See
// the class comment for why it exists. Its tone is not a constant -- it
// is whichever of black or white opposes the accent's own luminance.
//
// Scaled by DPI like every other dimension here, so it stays a visible
// hairline rather than a sub-pixel suggestion on a high-DPI display.
constexpr int kOutlineDip = 1;

// Strong, because it is one DIP wide with nothing else to help it.
// Tuned by eye and worth re-checking live (see PLAN.md's verification
// walkthrough) -- the kind of value only looking at it can settle.
constexpr int kOutlineAlpha = 210;

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

// A premultiplied top-down 32bpp BGRA pixel (alpha in the high byte) in
// `colorref`, which is in COLORREF's own 0x00BBGGRR layout.
//
// An earlier version took a bool and shortcut the multiply, because the
// colour was only ever pure white (a,a,a,a -- alpha broadcast into every
// byte) or pure black (alpha alone in the high byte). An arbitrary accent
// colour needs the real multiply. It costs almost nothing in practice:
// the bands, which are the overwhelming majority of the pixels, compute
// one value per row or column and fill_n/memcpy it, so this runs per
// *line*, not per pixel. Only the four small corner boxes call it per
// pixel, and they were already doing per-pixel sqrtf work.
uint32_t PremultipliedPixel(int alpha, uint32_t colorref) {
    const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0, 255));
    const uint32_t r = ((colorref & 0xFFu) * a) / 255u;
    const uint32_t g = (((colorref >> 8) & 0xFFu) * a) / 255u;
    const uint32_t b = (((colorref >> 16) & 0xFFu) * a) / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
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

// Logged on change only: ShowAroundTarget runs on every move of the
// target, and an elevated window being dragged would otherwise write a
// line per frame.
void ActiveWindowHalo::NotePlacement(int placement, HWND target) {
    if (placement == placementLogged_) {
        return;
    }
    placementLogged_ = placement;
    if (placement == static_cast<int>(HaloPlacement::AboveEverything)) {
        polish::LogDebug(std::format(
            L"[Polish] Halo: hwnd={} refused a z-order reference (elevated window), so the halo goes topmost "
            L"instead of behind it -- see PlaceHaloBehindTarget",
            reinterpret_cast<void*>(target)));
    } else if (placement == static_cast<int>(HaloPlacement::Failed)) {
        polish::LogDebug(std::format(L"[Polish] Halo: WARNING could not place the halo at all for hwnd={}",
                                     reinterpret_cast<void*>(target)));
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

    // Read fresh every render, never cached -- the same convention
    // IsDarkModeEnabled() carries, and for the same reason: the user can
    // change the accent in Settings while this is running.
    const COLORREF glow =
        halo_math::ClampGlowLuminance(GetAccentColor(), kGlowMinLuminance, kGlowMaxLuminance);
    const int glowWidth = targetWidth + inflate.left + inflate.right;
    const int glowHeight = targetHeight + inflate.top + inflate.bottom;

    const bool targetChanged = (target != lastTarget_);
    lastTarget_ = target;

    POINT dstPoint{targetRect.left - inflate.left, targetRect.top - inflate.top};

    // Cache key: (glow width, glow height, dpi, accent colour). Glow width/height
    // (not just the target's own size) already bakes in the per-side
    // inflation, so a target that crosses a monitor's flush edge without
    // changing its own size still correctly triggers a full re-render
    // here, even though CachedTargetSize() (target-size-only, for callers
    // debouncing resize bursts) would have reported "unchanged".
    const bool sameContent =
        (glowWidth == width_ && glowHeight == height_ && dpi == dpi_ && glow == glow_ && bits_ != nullptr);

    if (sameContent) {
        // Cheap move-only path: reposition without touching content
        // (documented UpdateLayeredWindow behavior when hdcSrc is null),
        // then just assert Z-order/visibility if either actually needs to
        // change -- SWP_NOZORDER otherwise, since nothing is changing
        // Z-order 60x/sec during a drag.
        UpdateLayeredWindow(window_, nullptr, &dstPoint, nullptr, nullptr, nullptr, 0, nullptr, 0);
        if (!visible_ || targetChanged) {
            NotePlacement(static_cast<int>(PlaceHaloBehindTarget(window_, target, SWP_SHOWWINDOW)), target);
            visible_ = true;
        } else {
            SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return;
    }

    const ULONGLONG renderStartMs = GetTickCount64();
    Render(targetWidth, targetHeight, inflate, dpi, glow);
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
    NotePlacement(static_cast<int>(PlaceHaloBehindTarget(window_, target, SWP_SHOWWINDOW)), target);
    visible_ = true;

    polish::LogDebug(std::format(
        L"[Polish] Halo: render target={} rect=({},{})-({},{}) dpi={} glow=0x{:06X} durationMs={}",
        reinterpret_cast<void*>(target), targetRect.left, targetRect.top, targetRect.right, targetRect.bottom, dpi,
        glow, renderDurationMs));
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

void ActiveWindowHalo::Render(int targetWidth, int targetHeight, RECT inflate, UINT dpi, COLORREF glow) {
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
    const int peak = kPeakAlpha;

    // Black under a light accent, white under a dark one -- whichever
    // opposes the glow. Derived from the colour actually being drawn, not
    // from the theme, so the pair always spans a real luminance range
    // however the accent is set.
    const uint32_t ringColor = halo_math::Luminance(glow) >= 128 ? 0x00000000u : 0x00FFFFFFu;

    // How far each band runs *past* the target's own edge, into pixels the
    // target itself covers -- see kUnderlapDip for why it exists at all.
    // Clamped against the corner radius, so an underlapped band can never
    // reach into the corner boxes' own rows/columns and fight them over
    // the same pixels, and against the target's own half-extents, so on a
    // tiny target two opposite bands can't both claim the middle.
    const int underlap = std::max(
        0, std::min({MulDiv(kUnderlapDip, static_cast<int>(dpi), 96), rInt, targetWidth / 2, targetHeight / 2}));

    const auto pixelPtr = [this](int x, int y) { return bits_ + static_cast<size_t>(y) * capWidth_ + x; };

    // The base peak-at-d<=0 falloff (halo_math::DistanceToAlpha), bounded
    // on the outside at d = halo (where the falloff has already reached 0
    // on its own) and on the inside at d = -underlap, past which the glow
    // sits far enough under its own target that not even the target's
    // semi-transparent border samples it.
    // The outline ring, in the opposite tone to the glow. Clamped to the
    // halo's own extent so a target with almost no room around it cannot
    // end up as a bare ring with no glow behind it.
    const int outlineThickness = std::min(std::max(1, MulDiv(kOutlineDip, static_cast<int>(dpi), 96)), halo);
    const int outlineAlpha = kOutlineAlpha;

    const auto colorForDistance = [&](float d) -> uint32_t {
        if (d >= static_cast<float>(halo) || d < -static_cast<float>(underlap)) {
            return 0;
        }
        // Checked before the glow: on the ring, the opposite tone wins
        // outright rather than being blended with the glow underneath.
        // A blend of pure white and pure black is grey, which reads as a
        // smudge at the window's edge instead of the crisp line this is
        // for -- and the ring is one DIP wide, so there is no room for a
        // gradient to say anything useful anyway.
        if (const int ring = halo_math::OutlineAlpha(d, outlineThickness, outlineAlpha); ring > 0) {
            return PremultipliedPixel(ring, ringColor);
        }
        return PremultipliedPixel(halo_math::DistanceToAlpha(d, halo, peak), glow);
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
        if (inflate.top > 0) {
            for (int y = 0; y < inflate.top + underlap; ++y) {
                const float d = std::fabs(static_cast<float>(y) - centerY) - halfHeight;
                std::fill_n(pixelPtr(midLeft, y), midRight - midLeft, colorForDistance(d));
            }
        }
        if (inflate.bottom > 0) {
            for (int y = tl.bottom - underlap; y < glowHeight; ++y) {
                const float d = std::fabs(static_cast<float>(y) - centerY) - halfHeight;
                std::fill_n(pixelPtr(midLeft, y), midRight - midLeft, colorForDistance(d));
            }
        }
    }

    // Left/right bands, across the flat vertical middle (y in [midTop,
    // midBottom)): same reduction along the other axis -- one value per
    // column, computed once into a small row buffer and replicated down
    // every row in range via memcpy rather than recomputed per row.
    if (midBottom > midTop) {
        if (inflate.left > 0) {
            const int bandWidth = inflate.left + underlap;
            std::vector<uint32_t> row(static_cast<size_t>(bandWidth));
            for (int x = 0; x < bandWidth; ++x) {
                const float d = std::fabs(static_cast<float>(x) - centerX) - halfWidth;
                row[static_cast<size_t>(x)] = colorForDistance(d);
            }
            for (int y = midTop; y < midBottom; ++y) {
                std::memcpy(pixelPtr(0, y), row.data(), row.size() * sizeof(uint32_t));
            }
        }
        if (inflate.right > 0) {
            const int bandWidth = inflate.right + underlap;
            const int bandLeft = tl.right - underlap;
            std::vector<uint32_t> row(static_cast<size_t>(bandWidth));
            for (int x = 0; x < bandWidth; ++x) {
                const float d = std::fabs(static_cast<float>(bandLeft + x) - centerX) - halfWidth;
                row[static_cast<size_t>(x)] = colorForDistance(d);
            }
            for (int y = midTop; y < midBottom; ++y) {
                std::memcpy(pixelPtr(bandLeft, y), row.data(), row.size() * sizeof(uint32_t));
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
    glow_ = glow;

    // Each one covers its band's underlapped extent too, not just the
    // pixels outside the target: those inner pixels are painted like any
    // other, so the next render has to clear them like any other.
    prevTopBand_ = RECT{0, 0, glowWidth, inflate.top + underlap};
    prevBottomBand_ = RECT{0, tl.bottom - underlap, glowWidth, glowHeight};
    prevLeftBand_ = RECT{0, inflate.top, inflate.left + underlap, tl.bottom};
    prevRightBand_ = RECT{tl.right - underlap, inflate.top, glowWidth, tl.bottom};
    hasPreviousBands_ = true;
}

}  // namespace polish

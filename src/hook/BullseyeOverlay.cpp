#include "hook/BullseyeOverlay.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

#include "util/DarkMode.h"

namespace polish {

namespace bullseye_math {

float EaseOutCubic(float t) {
    const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - u * u * u;
}

float RadiusAt(float t, float maxRadius, bool expanding) {
    const float e = EaseOutCubic(t);
    return maxRadius * (expanding ? e : 1.0f - e);
}

int AlphaEnvelope(float t, int peak) {
    constexpr float kRampEnd = 0.2f;
    constexpr float kFadeStart = 0.6f;
    if (t <= 0.0f || t >= 1.0f) {
        return 0;
    }
    float scale = 1.0f;
    if (t < kRampEnd) {
        scale = t / kRampEnd;
    } else if (t > kFadeStart) {
        const float f = (t - kFadeStart) / (1.0f - kFadeStart);
        scale = 1.0f - f * f * (3.0f - 2.0f * f);  // smoothstep down
    }
    return static_cast<int>(static_cast<float>(peak) * scale + 0.5f);
}

float RingDistance(float px, float py, float radius) {
    return std::fabs(std::sqrt(px * px + py * py) - radius);
}

int DistanceToAlpha(float d, float halfStroke, int peak) {
    if (halfStroke <= 0.0f) {
        return d <= 0.0f ? peak : 0;
    }
    const float t = std::clamp(d / halfStroke, 0.0f, 1.0f);
    const float falloff = (1.0f - t) * (1.0f - t);
    return static_cast<int>(static_cast<float>(peak) * falloff + 0.5f);
}

}  // namespace bullseye_math

namespace {

constexpr wchar_t kClassName[] = L"PolishBullseyeOverlay";

// All in device-independent pixels at 96 DPI, scaled per monitor in Start().
// Tuned by eye like ActiveWindowHalo's own constants -- adjust after seeing
// both themes live.
constexpr int kMaxRadiusDip = 180;
constexpr int kHalfStrokeDip = 12;
constexpr double kDurationMs = 340.0;

// Separate per theme for the same reason the halo's are: a black ring at
// a white ring's alpha reads much heavier.
constexpr int kPeakAlphaDark = 200;
constexpr int kPeakAlphaLight = 130;

// The ring is drawn in two tones: a core in the theme's own colour, and a
// contrasting outline just outside it.
//
// One colour cannot work. The theme only says what Windows is set to, not
// what is actually on screen underneath -- a white ring (dark mode) over a
// white document is invisible, which is exactly the case this fixes, and
// the reverse happens in light mode over dark content. Sampling the pixels
// behind the ring would adapt, but it means capturing the screen on every
// copy and still picks one colour for a ring that can span content of
// several shades.
//
// A light/dark pair instead is always legible against anything, because
// whatever the background is, one of the two contrasts with it. This is
// how mouse cursors and focus rings stay visible everywhere, and it needs
// no knowledge of the background at all.
//
// Extends the ring outward rather than thinning the core, so the shape
// keeps the weight it was tuned to. Alpha is lower than the core's: the
// outline is an edge that makes the core readable, not a second ring.
constexpr int kOutlineHalfStrokeDip = 4;
constexpr float kOutlineAlphaScale = 0.62f;

void EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // UpdateLayeredWindow bypasses WM_PAINT entirely -- see
    // ActiveWindowHalo's EnsureClassRegistered.
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

// Same single-store premultiply as ActiveWindowHalo.cpp's
// PremultipliedPixel (duplicated rather than exported: it's three lines,
// and the two classes are deliberately independent). Premultiplied white
// is alpha broadcast into every byte; premultiplied black is alpha alone
// in the high byte.
uint32_t PremultipliedPixel(int alpha, bool white) {
    const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0, 255));
    return white ? a * 0x01010101u : (a << 24);
}

// The core composited over its contrasting outline, as one premultiplied
// pixel. Standard source-over: the core is opaque white or black, so the
// colour channels collapse to whichever of the two is the light one.
uint32_t TwoTonePixel(int coreAlpha, int outlineAlpha, bool coreIsWhite) {
    const float core = static_cast<float>(std::clamp(coreAlpha, 0, 255)) / 255.0f;
    const float outline = static_cast<float>(std::clamp(outlineAlpha, 0, 255)) / 255.0f;
    const float combined = core + outline * (1.0f - core);
    // Only the white layer contributes colour; the black one contributes
    // opacity alone. Either way the result stays <= combined, so the pixel
    // remains valid premultiplied data.
    const float light = coreIsWhite ? core : outline * (1.0f - core);
    const auto toByte = [](float v) {
        return static_cast<uint32_t>(std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
    };
    return (toByte(combined) << 24) | (toByte(light) << 16) | (toByte(light) << 8) | toByte(light);
}

}  // namespace

BullseyeOverlay::BullseyeOverlay(HINSTANCE instance) {
    EnsureClassRegistered(instance);
    // WS_EX_TOOLWINDOW keeps it out of Task View / Alt+Tab; WS_EX_TOPMOST is
    // the deliberate divergence from the halo (see the class comment). No
    // owner -- same cross-process input-queue reasoning as the halo.
    const DWORD exStyle =
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    window_ = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    QueryPerformanceFrequency(&frequency_);
}

BullseyeOverlay::~BullseyeOverlay() {
    if (window_ != nullptr && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
    }
}

void BullseyeOverlay::Hide() {
    if (window_ != nullptr && visible_) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
    active_ = false;
}

void BullseyeOverlay::Start(BullseyePhase phase, POINT screenPoint) {
    if (window_ == nullptr) {
        return;
    }

    // DPI comes from the monitor under the point -- our own visual, drawn
    // in raw screen pixels (see ActiveWindowHalo::ShowAroundTarget).
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr) {
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    }
    const int dpi = static_cast<int>(dpiX);

    maxRadius_ = static_cast<float>(MulDiv(kMaxRadiusDip, dpi, 96));
    halfStroke_ = static_cast<float>(MulDiv(kHalfStrokeDip, dpi, 96));
    outlineHalfStroke_ = static_cast<float>(MulDiv(kOutlineHalfStrokeDip, dpi, 96));
    // Two pixels of slack so the outermost antialiased pixels are never
    // clipped by the window edge. Sized off the outline, which is the
    // outermost thing drawn -- sizing off the core alone would clip it.
    size_ = 2 * static_cast<int>(std::ceil(maxRadius_ + halfStroke_ + outlineHalfStroke_)) + 2;
    origin_ = POINT{screenPoint.x - size_ / 2, screenPoint.y - size_ / 2};

    EnsureDibCapacity(size_, size_);
    if (bits_ == nullptr) {
        active_ = false;
        return;
    }

    phase_ = phase;
    QueryPerformanceCounter(&startCounter_);
    active_ = true;
    needsShow_ = true;
}

bool BullseyeOverlay::AdvanceFrame() {
    if (!active_ || window_ == nullptr || bits_ == nullptr) {
        return false;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double elapsedMs =
        static_cast<double>(now.QuadPart - startCounter_.QuadPart) * 1000.0 / static_cast<double>(frequency_.QuadPart);
    const float progress = static_cast<float>(elapsedMs / kDurationMs);
    if (progress >= 1.0f) {
        Hide();
        return false;
    }

    RenderFrame(progress, IsDarkModeEnabled());

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, dib_));

    POINT srcPoint{0, 0};
    SIZE size{size_, size_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screenDC, &origin_, &size, memDC, &srcPoint, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (needsShow_) {
        // After UpdateLayeredWindow, so the first thing ever shown is
        // already this animation's content at its own position, never a
        // stale frame of the previous one.
        SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        needsShow_ = false;
        visible_ = true;
    }
    return true;
}

void BullseyeOverlay::EnsureDibCapacity(int width, int height) {
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

    // A fresh DIB is zeroed -- nothing stale for the next frame to clear.
    prevDirty_ = RECT{};
}

void BullseyeOverlay::ClearRect(const RECT& rect) {
    if (bits_ == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    for (int y = rect.top; y < rect.bottom; ++y) {
        std::fill_n(bits_ + static_cast<size_t>(y) * capWidth_ + rect.left, rect.right - rect.left, 0u);
    }
}

void BullseyeOverlay::RenderFrame(float progress, bool isDark) {
    ClearRect(prevDirty_);
    prevDirty_ = RECT{};

    const int peak = bullseye_math::AlphaEnvelope(progress, isDark ? kPeakAlphaDark : kPeakAlphaLight);
    if (peak <= 0) {
        return;
    }

    const float radius = bullseye_math::RadiusAt(progress, maxRadius_, phase_ == BullseyePhase::Paste);
    // The outline sits outside the core, so it -- not the core -- sets the
    // bounds everything below is clipped and span-skipped against.
    const float outlineHalfStroke = halfStroke_ + outlineHalfStroke_;
    const int outlinePeak = static_cast<int>(static_cast<float>(peak) * kOutlineAlphaScale);
    const float outer = radius + outlineHalfStroke;
    const float inner = radius - outlineHalfStroke;
    const float center = static_cast<float>(size_) / 2.0f;

    const int yMin = std::max(0, static_cast<int>(std::floor(center - outer)));
    const int yMax = std::min(size_, static_cast<int>(std::ceil(center + outer)));
    const int xMin = yMin;  // the buffer is square and the ring centred, so both axes share bounds
    const int xMax = yMax;

    for (int y = yMin; y < yMax; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - center;
        if (std::fabs(dy) >= outer) {
            continue;
        }
        const float outerSpan = std::sqrt(outer * outer - dy * dy);
        const int rowLeft = std::max(xMin, static_cast<int>(std::floor(center - outerSpan)));
        const int rowRight = std::min(xMax, static_cast<int>(std::ceil(center + outerSpan)));

        // Skip the hole: pixels strictly inside the inner circle are
        // farther than halfStroke from the centre line, so they'd get
        // alpha 0 anyway -- this is what keeps the cost O(perimeter *
        // stroke) rather than O(bounding-box area). Ends are widened by a
        // pixel so the skip is conservative; the per-pixel alpha still
        // decides what's actually drawn.
        int leftEnd = rowRight;
        int rightStart = rowRight;
        if (inner > 0.0f && std::fabs(dy) < inner) {
            const float innerSpan = std::sqrt(inner * inner - dy * dy);
            leftEnd = std::clamp(static_cast<int>(std::ceil(center - innerSpan)) + 1, rowLeft, rowRight);
            rightStart = std::clamp(static_cast<int>(std::floor(center + innerSpan)) - 1, leftEnd, rowRight);
        }

        uint32_t* row = bits_ + static_cast<size_t>(y) * capWidth_;
        auto fillSpan = [&](int x0, int x1) {
            for (int x = x0; x < x1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - center;
                const float d = bullseye_math::RingDistance(dx, dy, radius);
                const int coreAlpha = bullseye_math::DistanceToAlpha(d, halfStroke_, peak);
                const int outlineAlpha = bullseye_math::DistanceToAlpha(d, outlineHalfStroke, outlinePeak);
                if (coreAlpha > 0 || outlineAlpha > 0) {
                    row[x] = TwoTonePixel(coreAlpha, outlineAlpha, /*coreIsWhite=*/isDark);
                }
            }
        };
        fillSpan(rowLeft, leftEnd);
        fillSpan(rightStart, rowRight);
    }

    prevDirty_ = RECT{xMin, yMin, xMax, yMax};
}

}  // namespace polish

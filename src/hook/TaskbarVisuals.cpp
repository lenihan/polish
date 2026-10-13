#include "hook/TaskbarVisuals.h"

#include <algorithm>

namespace polish {

namespace {

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

}  // namespace

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

void DrawTaskbarFill(Gdiplus::Graphics& graphics, const Gdiplus::RectF& cell, UINT dpi, float strength) {
    const auto scale = [dpi](int dip) { return static_cast<float>(MulDiv(dip, static_cast<int>(dpi), 96)); };
    const float insetX = scale(kHighlightInsetXDip);
    const float insetY = scale(kHighlightInsetYDip);
    const float radius = scale(kHighlightRadiusDip);
    const float x = cell.X + insetX;
    const float y = cell.Y + insetY;
    const float w = cell.Width - insetX * 2;
    const float h = cell.Height - insetY * 2;
    if (w <= 0 || h <= 0) {
        return;
    }
    const bool dark = TaskbarIsDark();
    const float base = dark ? kHighlightAlphaDark : kHighlightAlphaLight;
    const BYTE alpha = static_cast<BYTE>(std::clamp(base * strength + 0.5f, 0.0f, 255.0f));
    const BYTE tone = dark ? 255 : 0;
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, x, y, w, h, std::min(radius, std::min(w, h) / 2));
    Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, tone, tone, tone));
    graphics.FillPath(&brush, &path);
}

}  // namespace polish

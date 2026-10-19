#include "util/DarkMode.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace polish {

namespace {

// Some SDK headers don't yet define this (added Windows 10 20H1) --
// the numeric value is stable/documented, safe to fall back to.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif

// uxtheme.dll's dark-mode support has no header/import-lib entry --
// only reachable via GetProcAddress on these two ordinals, same as
// every other dark-mode-aware Win32 app without an owner-drawn menu.
using SetPreferredAppModeFn = int(WINAPI*)(int);
using FlushMenuThemesFn = void(WINAPI*)();

enum PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };

// Shared by ApplyDarkModeToMenu and ApplyDarkModeToTooltip -- both need
// this process opted into *tracking* the live system dark/light setting
// (AllowDark, not ForceDark) before SetWindowTheme's "DarkMode_Explorer"
// pseudo-theme has any effect, and both need FlushMenuThemes so a
// mid-session theme change is picked up rather than whatever was cached
// from the last time a themed control was shown.
void EnsureDarkModeAllowed() {
    static HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme == nullptr) {
        return;
    }
    static bool appModeSet = false;
    if (!appModeSet) {
        if (auto* setPreferredAppMode =
                reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)))) {
            setPreferredAppMode(AllowDark);
        }
        appModeSet = true;
    }
    if (auto* flushMenuThemes =
            reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)))) {
        flushMenuThemes();
    }
}

}  // namespace

bool IsDarkModeEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ,
                       &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<BYTE*>(&value),
                                      &size) == ERROR_SUCCESS &&
                     type == REG_DWORD;
    RegCloseKey(key);
    return ok && value == 0;
}

void ApplyDarkTitleBar(HWND hwnd, bool dark) {
    BOOL enabled = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
}

void ApplyDarkModeToMenu(HWND ownerWindow) {
    EnsureDarkModeAllowed();
    SetWindowTheme(ownerWindow, IsDarkModeEnabled() ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void ApplyDarkModeToTooltip(HWND tooltipWindow) {
    EnsureDarkModeAllowed();
    SetWindowTheme(tooltipWindow, IsDarkModeEnabled() ? L"DarkMode_Explorer" : nullptr, nullptr);
}

namespace {

bool ReadAccentValue(const wchar_t* subKey, const wchar_t* name, COLORREF* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
                        ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    if (ok) {
        *out = static_cast<COLORREF>(value & 0x00FFFFFF);  // 0xAABBGGRR, COLORREF's own order
    }
    return ok;
}

}  // namespace

// Entry 3 of Explorer\Accent\AccentPalette: the "Accent" shade, which is what
// UISettings reports as UIColorType::Accent. The palette is eight RGBA
// entries (light3..light1, accent, dark1..dark3), 4 bytes each.
static bool ReadAccentPalette(COLORREF* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    BYTE palette[32]{};
    DWORD size = sizeof(palette);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, L"AccentPalette", nullptr, &type, palette, &size) == ERROR_SUCCESS &&
                    type == REG_BINARY && size >= 16;
    RegCloseKey(key);
    if (ok) {
        *out = RGB(palette[12], palette[13], palette[14]);
    }
    return ok;
}

COLORREF GetAccentColor() {
    // The palette first. The values below can sit at a neutral grey while the
    // palette -- and the accent the user actually sees -- is their real colour
    // (seen live: AccentColorMenu and DWM\AccentColor both 0x727272 beside a
    // 0078D4 blue palette).
    //
    // After it, Explorer\Accent\AccentColorMenu, then DWM\AccentColor, which
    // is only the *title bar* accent and goes grey when "Show accent color on
    // title bars" is off.
    COLORREF color = 0;
    if (ReadAccentPalette(&color)) {
        return color;
    }
    if (ReadAccentValue(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentColorMenu",
                        &color) ||
        ReadAccentValue(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", &color)) {
        return color;
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

}  // namespace polish

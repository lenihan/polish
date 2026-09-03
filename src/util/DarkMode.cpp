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
    static HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme == nullptr) {
        return;
    }
    // AllowDark (not ForceDark) once, ever -- opts this process into
    // *tracking* the live system setting for native menus, rather than
    // always being dark regardless of it.
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
    SetWindowTheme(ownerWindow, IsDarkModeEnabled() ? L"DarkMode_Explorer" : nullptr, nullptr);
}

}  // namespace polish

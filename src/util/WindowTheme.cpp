#include "util/WindowTheme.h"

#include <uxtheme.h>

#include "util/DarkMode.h"

namespace polish {

namespace {

constexpr COLORREF kBackground = RGB(32, 32, 32);
constexpr COLORREF kEditBackground = RGB(45, 45, 45);
constexpr COLORREF kText = RGB(240, 240, 240);

}  // namespace

WindowTheme::~WindowTheme() {
    if (background_ != nullptr) {
        DeleteObject(background_);
    }
    if (editBackground_ != nullptr) {
        DeleteObject(editBackground_);
    }
}

// Controls get the same "DarkMode_Explorer" pseudo-theme the menus and
// tooltips use; the background and the static/edit colours are ours (see
// Handle). Static controls are left unthemed -- they have no theme to apply
// and are coloured through WM_CTLCOLORSTATIC.
void WindowTheme::Apply(HWND window) {
    dark_ = IsDarkModeEnabled();
    if (background_ == nullptr) {
        background_ = CreateSolidBrush(kBackground);
        editBackground_ = CreateSolidBrush(kEditBackground);
    }
    ApplyDarkTitleBar(window, dark_);
    // Opts the process in to dark theming (SetWindowTheme does nothing
    // otherwise) and themes the window itself.
    ApplyDarkModeToMenu(window);
    const wchar_t* theme = dark_ ? L"DarkMode_Explorer" : nullptr;
    for (HWND child = GetWindow(window, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t className[16] = L"";
        GetClassNameW(child, className, 16);
        if (_wcsicmp(className, L"STATIC") != 0) {
            SetWindowTheme(child, theme, nullptr);
        }
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

bool WindowTheme::Handle(HWND window, UINT message, WPARAM wParam, LRESULT* result) {
    if (!dark_) {
        return false;
    }
    switch (message) {
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, background_);
            *result = 1;
            return true;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kText);
            SetBkMode(dc, TRANSPARENT);
            *result = reinterpret_cast<LRESULT>(background_);
            return true;
        }
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kText);
            SetBkColor(dc, kEditBackground);
            *result = reinterpret_cast<LRESULT>(editBackground_);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace polish

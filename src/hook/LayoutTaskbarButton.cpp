#include "hook/LayoutTaskbarButton.h"

#include <objbase.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cstring>
#include <utility>

#include "resource.h"
#include "util/Logging.h"

namespace polish {

namespace {

constexpr wchar_t kClassName[] = L"PolishLayoutTaskbar";
constexpr wchar_t kAppId[] = L"Polish.Layout";

}  // namespace

const wchar_t* LayoutTaskbarButton::AppId() {
    return kAppId;
}

LRESULT CALLBACK LayoutTaskbarButton::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    LayoutTaskbarButton* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<LayoutTaskbarButton*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<LayoutTaskbarButton*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    switch (message) {
        case WM_ACTIVATE:
            // A click on the button: the shell activated the window. Also
            // seen when something else activates it by accident, which is
            // harmless -- the owner just opens the menu.
            if (LOWORD(wParam) != WA_INACTIVE && self != nullptr && self->onClicked_) {
                self->onClicked_();
            }
            return 0;
        case WM_SYSCOMMAND:
            // Never really shown, so minimizing or restoring it means
            // nothing in itself. The shell sends these when the button is
            // clicked while the window is already in front; a restore is a
            // click like any other, a minimize is simply dropped.
            if ((wParam & 0xFFF0) == SC_RESTORE) {
                if (self != nullptr && self->onClicked_) {
                    self->onClicked_();
                }
                return 0;
            }
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                return 0;
            }
            break;
        case WM_CLOSE:
            // "Close window" on the button's jump list. Never destroyed
            // here -- the owner decides, and does it later.
            if (self != nullptr && self->onCloseRequested_) {
                self->onCloseRequested_();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LayoutTaskbarButton::LayoutTaskbarButton(HINSTANCE instance, std::function<void()> onClicked,
                                         std::function<void()> onCloseRequested)
    : onClicked_(std::move(onClicked)), onCloseRequested_(std::move(onCloseRequested)) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kClassName;
        registered = RegisterClassExW(&windowClass) != 0;
    }
    // WS_EX_APPWINDOW forces a taskbar button; the window is one pixel,
    // offscreen, and never painted.
    window_ = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"Layout", WS_POPUP, -32000, -32000, 1, 1, nullptr,
                              nullptr, instance, this);
    if (window_ == nullptr) {
        LogDebug(L"[Polish] Layout button: window creation failed");
        return;
    }
    if (HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_POLISH_TRAY))) {
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
    // An AppUserModelID of its own, so the shell gives this its own button
    // rather than grouping it under Polish's other windows.
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(window_, IID_PPV_ARGS(&store))) && store) {
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_LPWSTR;
        const size_t bytes = (wcslen(kAppId) + 1) * sizeof(wchar_t);
        value.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (value.pwszVal != nullptr) {
            memcpy(value.pwszVal, kAppId, bytes);
            store->SetValue(PKEY_AppUserModel_ID, value);
            store->Commit();
        }
        PropVariantClear(&value);
    }
    ShowWindow(window_, SW_SHOWNOACTIVATE);
}

LayoutTaskbarButton::~LayoutTaskbarButton() {
    if (window_ != nullptr) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
    }
}

}  // namespace polish

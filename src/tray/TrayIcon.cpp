#include "tray/TrayIcon.h"

#include <algorithm>

#include <shellapi.h>

#include "resource.h"
#include "util/DarkMode.h"

namespace polish {

namespace {
constexpr UINT kIconId = 1;
}  // namespace

TrayIcon::TrayIcon(HWND messageWindow, std::function<void(HMENU)> populateMenu,
                    std::function<void(UINT)> onCommand, std::wstring tooltip)
    : messageWindow_(messageWindow),
      populateMenu_(std::move(populateMenu)),
      onCommand_(std::move(onCommand)),
      tooltip_(std::move(tooltip)) {
    AddIcon();
}

TrayIcon::~TrayIcon() {
    if (iconAdded_) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = messageWindow_;
        data.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
}

void TrayIcon::AddIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = messageWindow_;
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_POLISH_TRAY));
    // szTip holds 128 wchar_t including the terminator, and the shell
    // simply drops a tip that does not fit rather than truncating it for
    // you -- so truncate here, where it is obvious why.
    const size_t cap = ARRAYSIZE(data.szTip) - 1;
    const size_t length = std::min(tooltip_.size(), cap);
    std::copy_n(tooltip_.begin(), length, data.szTip);
    data.szTip[length] = 0;

    iconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
}

void TrayIcon::ShowNotification(const std::wstring& title, const std::wstring& text) {
    if (!iconAdded_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = messageWindow_;
    data.uID = kIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    // The shell silently drops a string that does not fit, so truncate.
    const size_t titleLength = std::min(title.size(), ARRAYSIZE(data.szInfoTitle) - 1);
    std::copy_n(title.begin(), titleLength, data.szInfoTitle);
    data.szInfoTitle[titleLength] = 0;
    const size_t textLength = std::min(text.size(), ARRAYSIZE(data.szInfo) - 1);
    std::copy_n(text.begin(), textLength, data.szInfo);
    data.szInfo[textLength] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::HandleTaskbarRecreated() {
    iconAdded_ = false;
    AddIcon();
}

void TrayIcon::HandleCallbackMessage(LPARAM lParam) {
    const UINT mouseMessage = static_cast<UINT>(lParam);
    if (mouseMessage == WM_RBUTTONUP || mouseMessage == WM_LBUTTONUP || mouseMessage == WM_CONTEXTMENU) {
        ShowContextMenu();
    }
}

void TrayIcon::HandleCommand(WPARAM wParam) {
    if (onCommand_) {
        onCommand_(LOWORD(wParam));
    }
}

void TrayIcon::ShowContextMenu() {
    POINT cursorPos;
    GetCursorPos(&cursorPos);

    HMENU menu = CreatePopupMenu();
    if (populateMenu_) {
        populateMenu_(menu);
    }
    ApplyDarkModeToMenu(messageWindow_);

    // Standard dance so the menu dismisses correctly on an outside click:
    // the window must be foreground before TrackPopupMenuEx, and a
    // trailing WM_NULL ensures the menu closes if the user clicks away.
    SetForegroundWindow(messageWindow_);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, cursorPos.x, cursorPos.y, messageWindow_, nullptr);
    PostMessageW(messageWindow_, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

}  // namespace polish

#include "tray/TrayIcon.h"

#include <shellapi.h>

#include "resource.h"

namespace polish {

namespace {
constexpr UINT kIconId = 1;
}  // namespace

TrayIcon::TrayIcon(HWND messageWindow, std::function<void()> onExitRequested)
    : messageWindow_(messageWindow), onExitRequested_(std::move(onExitRequested)) {
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
    wcscpy_s(data.szTip, L"Polish (running) - keeps Snap and Maximize/Restore in sync");

    iconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
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
    if (LOWORD(wParam) == kExitCommandId && onExitRequested_) {
        onExitRequested_();
    }
}

void TrayIcon::ShowContextMenu() {
    POINT cursorPos;
    GetCursorPos(&cursorPos);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kExitCommandId, L"Exit");

    // Standard dance so the menu dismisses correctly on an outside click:
    // the window must be foreground before TrackPopupMenuEx, and a
    // trailing WM_NULL ensures the menu closes if the user clicks away.
    SetForegroundWindow(messageWindow_);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, cursorPos.x, cursorPos.y, messageWindow_, nullptr);
    PostMessageW(messageWindow_, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

}  // namespace polish

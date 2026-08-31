#pragma once

#include <windows.h>

#include <functional>

namespace polish {

// A minimal system tray presence: an icon (so it's visible that the app
// is running at all -- there is otherwise no window to look at) and a
// right-click context menu whose actual content the caller owns.
class TrayIcon {
public:
    // `messageWindow` receives the tray callback message; the caller's
    // WindowProc must route it (message id == kCallbackMessage) to
    // HandleCallbackMessage, and route WM_COMMAND to HandleCommand.
    //
    // populateMenu(menu): called each time the context menu is about to
    //   be shown -- build fresh content directly on the given (empty)
    //   HMENU via AppendMenuW, including up-to-date checkbox state
    //   (MF_CHECKED) for anything toggleable. TrayIcon owns none of the
    //   content or command IDs; that's entirely the caller's.
    // onCommand(commandId): routes a selected menu item's ID back to the
    //   caller (including whatever the caller used for its own Exit
    //   item -- TrayIcon no longer has an opinion on that).
    TrayIcon(HWND messageWindow, std::function<void(HMENU)> populateMenu,
             std::function<void(UINT commandId)> onCommand);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Routes the tray callback message (right-click, etc.).
    void HandleCallbackMessage(LPARAM lParam);

    // Routes WM_COMMAND from the context menu to onCommand.
    void HandleCommand(WPARAM wParam);

    // Re-adds the icon after explorer.exe restarts. Callers should watch
    // for RegisterWindowMessageW(L"TaskbarCreated") and call this then.
    void HandleTaskbarRecreated();

    static constexpr UINT kCallbackMessage = WM_APP + 1;

private:
    void AddIcon();
    void ShowContextMenu();

    HWND messageWindow_;
    std::function<void(HMENU)> populateMenu_;
    std::function<void(UINT)> onCommand_;
    bool iconAdded_ = false;
};

}  // namespace polish

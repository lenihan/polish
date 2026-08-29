#pragma once

#include <windows.h>

#include <functional>

namespace polish {

// A minimal system tray presence: an icon (so it's visible that the app
// is running at all -- there is otherwise no window to look at) and a
// right-click context menu with an Exit item (so it can be closed
// without Task Manager). Icon/branding polish and autostart are Phase 3
// work; this is the bare minimum for "I can tell it's running and I can
// turn it off."
class TrayIcon {
public:
    // `messageWindow` receives the tray callback message; the caller's
    // WindowProc must route it (message id == kCallbackMessage) to
    // HandleCallbackMessage, and must route WM_COMMAND with
    // kExitCommandId to onExitRequested itself (TrackPopupMenu posts
    // WM_COMMAND to messageWindow, not back through this class).
    TrayIcon(HWND messageWindow, std::function<void()> onExitRequested);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Routes the tray callback message (right-click, etc.).
    void HandleCallbackMessage(LPARAM lParam);

    // Routes WM_COMMAND from the context menu; invokes onExitRequested if
    // wParam's low word is kExitCommandId.
    void HandleCommand(WPARAM wParam);

    // Re-adds the icon after explorer.exe restarts. Callers should watch
    // for RegisterWindowMessageW(L"TaskbarCreated") and call this then.
    void HandleTaskbarRecreated();

    static constexpr UINT kCallbackMessage = WM_APP + 1;
    static constexpr UINT kExitCommandId = 1;

private:
    void AddIcon();
    void ShowContextMenu();

    HWND messageWindow_;
    std::function<void()> onExitRequested_;
    bool iconAdded_ = false;
};

}  // namespace polish

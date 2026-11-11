#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <vector>

#include "settings/SessionStore.h"
#include "util/WindowTheme.h"

namespace polish {

// "Reopen your apps": a list of windows that were open last time, each with a
// checkbox and all ticked. OK returns the ticked ones, Cancel returns nothing.
//
// A modal window of its own, in the style of the other Polish dialogs: system
// font, the Explorer visual style on the list (which is what gives it the
// Windows 11 selection and checkbox look), rounded corners from the OS, and
// the system light/dark theme.
class RestoreAppsDialog {
public:
    explicit RestoreAppsDialog(HINSTANCE instance);
    ~RestoreAppsDialog();

    RestoreAppsDialog(const RestoreAppsDialog&) = delete;
    RestoreAppsDialog& operator=(const RestoreAppsDialog&) = delete;

    // Says which of the given apps are still not open (true = still missing),
    // one flag per app, in order.
    using MissingCheck = std::function<std::vector<bool>(const std::vector<SavedApp>&)>;

    // Blocks in its own message loop. nullopt on Cancel (or closing it);
    // otherwise the apps still ticked, in list order. While it is up,
    // `stillMissing` is asked once a second, and an app that has been opened
    // since (by the user, or by something starting with Windows) is taken off the
    // list. When the list empties the dialog closes.
    std::optional<std::vector<SavedApp>> ShowModal(const std::vector<SavedApp>& apps, MissingCheck stillMissing = {});

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls(HWND hwnd);
    void LayoutControls();
    void FillList();
    void Commit();
    void SetCheckedAll(bool checked);
    void DropOpenedApps();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    HFONT headingFont_ = nullptr;
    WindowTheme theme_;
    HWND heading_ = nullptr;
    HWND hint_ = nullptr;
    HWND list_ = nullptr;
    HWND selectAll_ = nullptr;
    HWND selectNone_ = nullptr;
    HWND okButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    std::vector<SavedApp> apps_;
    std::optional<std::vector<SavedApp>> result_;
    MissingCheck stillMissing_;
    bool done_ = false;
};

}  // namespace polish

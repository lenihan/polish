#pragma once

#include <windows.h>

#include <functional>
#include <vector>

#include "settings/FeatureCatalog.h"

namespace polish {

// The Settings window: one checkbox per feature in the FeatureCatalog,
// each with its plain-language description under it.
//
// Modeless, unlike StackHotkeyDialog -- it is a place to leave open while
// you try things, and a toggle takes effect the moment it is clicked, so
// there is no Save/Cancel. It owns no state of its own: it asks the app
// whether a feature is on, and tells the app when the user flips one (the
// app's ApplyFeatureToggle, the same path the tray menu takes). Closing it
// hides it; the next Show() refreshes the checkboxes, since the tray menu
// may have changed them meanwhile.
class SettingsWindow {
public:
    using IsEnabledFn = std::function<bool(Feature)>;
    using SetEnabledFn = std::function<void(Feature, bool)>;

    SettingsWindow(HINSTANCE instance, IsEnabledFn isEnabled, SetEnabledFn setEnabled);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Creates the window on first use, then shows it and brings it to the
    // front.
    void Show();

    // Gives the window first look at a message of the main loop, for Tab
    // and Esc. True if it was consumed.
    bool PreTranslate(MSG& msg);

private:
    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void Refresh();

    struct Row {
        Feature feature;
        HWND check = nullptr;
        HWND description = nullptr;
    };

    HINSTANCE instance_;
    IsEnabledFn isEnabled_;
    SetEnabledFn setEnabled_;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<Row> rows_;
};

}  // namespace polish

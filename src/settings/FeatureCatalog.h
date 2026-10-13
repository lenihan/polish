#pragma once

#include "settings/Settings.h"

#include <vector>

namespace polish {

// Every on/off feature Polish has, in one table: its id, what the user
// sees it called, a plain-language description, and which Settings member
// holds its switch.
//
// One table because the same facts are wanted in three places that used to
// each carry their own copy -- the tray menu's labels, the toggle handling,
// and (now) the Settings window's pages -- and a feature renamed in one of
// them and not the others is exactly the drift this prevents. It is data
// only: what *turning a feature on or off does* is main.cpp's
// ApplyFeatureToggle, because that needs the app's live state.
enum class Feature {
    RestoreSync,
    AltTab,
    Halo,
    Bullseye,
    KeepClipboard,
    Taskbar,
    MoveMode,
    Scale,
    PolishButton,
};

struct FeatureInfo {
    Feature id;
    // Short and in the voice of the menu item it is shown as.
    const wchar_t* name;
    // One or two sentences for the Settings window, written for someone who
    // has never seen the feature: what it does and why it is there.
    const wchar_t* description;
    // The Settings member that holds this feature's switch.
    bool Settings::* member;
};

// All features, in the order the Settings window lists them.
const std::vector<FeatureInfo>& AllFeatures();

// The entry for `id`. Always present for every Feature value.
const FeatureInfo& FeatureFor(Feature id);

bool IsFeatureEnabled(const Settings& settings, Feature id);
void SetFeatureEnabled(Settings& settings, Feature id, bool enabled);

// The same idea for the global hotkeys: each one's name and the pair of
// Settings members (RegisterHotKey modifiers and virtual key) that hold its
// combination. What re-registering one *does* is main.cpp's, for the
// reason ApplyFeatureToggle is.
enum class Hotkey {
    NewStack,
    TileTwoWay,
    TileThreeWay,
    TileFourWay,
    LayoutMenu,
    ScaleDown,
    ScaleUp,
    ScaleReset,
};

struct HotkeyInfo {
    Hotkey id;
    const wchar_t* name;
    UINT Settings::* modifiers;
    UINT Settings::* virtualKey;
};

// All hotkeys, in the order the Settings window lists them.
const std::vector<HotkeyInfo>& AllHotkeys();
const HotkeyInfo& HotkeyFor(Hotkey id);

struct HotkeyCombination {
    UINT modifiers = 0;
    UINT virtualKey = 0;
};

HotkeyCombination GetHotkey(const Settings& settings, Hotkey id);
void SetHotkey(Settings& settings, Hotkey id, HotkeyCombination combination);

}  // namespace polish

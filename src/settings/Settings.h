#pragma once

#include <windows.h>

namespace polish {

// Persisted, user-toggleable feature flags -- stored in the registry
// (HKCU\Software\Polish), not a config file. There's no other reason
// this app would need to touch the filesystem, and the registry gives
// atomic per-value read/write for free.
struct Settings {
    bool restoreSyncEnabled = true;
    bool altTabEnabled = true;
    // The soft glow drawn around the active window's own edge (see
    // ActiveWindowHalo) -- on by default like every other feature toggle
    // here, since it's meant to help by default, not be discovered.
    bool haloEnabled = true;
    // The copy/paste ring animation (see BullseyeOverlay).
    bool bullseyeEnabled = true;
    // Polish's taskbar behavior: its own window list on hover instead of
    // the native thumbnail flyout, and click-to-cycle an app's windows in
    // MRU order (see TaskbarShield and TaskbarHook). The one toggle here
    // that changes how another process's UI behaves rather than only
    // adding to it, so switching it off must restore the native taskbar
    // completely -- see main.cpp's ApplyTaskbarSetting.
    bool taskbarEnabled = true;

    // The "New Group" global hotkey -- RegisterHotKey's own modifier
    // flags (MOD_ALT/MOD_CONTROL/MOD_SHIFT/MOD_WIN, OR'd together) and
    // a single virtual-key code. Default Win+Alt+G. User-configurable
    // (see GroupHotkeyDialog) because a machine can already have that
    // combination claimed by something else -- confirmed happening on
    // the dev machine itself.
    UINT groupHotkeyModifiers = MOD_WIN | MOD_ALT;
    UINT groupHotkeyVirtualKey = 'G';
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Autostart is deliberately NOT part of Settings/the registry values
// above -- its single source of truth is the Run key's own presence.
// IsStartAtLoginEnabled queries it live rather than trusting a cached
// bool, so a user who removes it via Windows' own Startup Apps settings
// doesn't leave Polish's idea of the setting stale.
bool IsStartAtLoginEnabled();
void SetStartAtLoginEnabled(bool enabled);

}  // namespace polish

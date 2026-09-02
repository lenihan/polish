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

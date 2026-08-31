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

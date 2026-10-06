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

    // Easy move/resize mode: hold Win and the whole window becomes a
    // move target, with the right button resizing by the grabbed corner
    // quadrant (see hook/MoveModeHook.h). Off makes the Win hold
    // indistinguishable from the hook not existing, because this one
    // claims a key Windows itself uses heavily.
    bool moveModeEnabled = true;
    // How close an edge has to come to a snap target before it is pulled
    // in, in physical pixels. Stored rather than fixed because the right
    // value depends on pointer speed and monitor DPI, and the default
    // (windowtracking/MoveSnap.h's kDefaultSnapThresholdPx) is a guess
    // tuned on one machine. No UI for it yet -- registry only.
    UINT moveModeSnapThresholdPx = 12;

    // The "Tile 2-way", "Tile 3-way" and "Tile 4-way" global hotkeys, in
    // the same RegisterHotKey form as the stack one below. Stored rather
    // than fixed for the reason that one documents: a combination can
    // already be claimed by something else on a given machine, which was
    // confirmed happening here. No picker dialog for these yet -- change
    // them in HKCU\Software\Polish if they collide.
    //
    // The digit matches the number of windows, which makes three hotkeys
    // as easy to remember as one. Ctrl+Alt rather than Win+Alt, which
    // would have matched the stack hotkey below: every Win-based pair
    // tried had at least one half already claimed on the dev machine
    // (Win+Alt+T, Win+Shift+T, Win+Shift+C and Win+Ctrl+C were all
    // taken), and a pair that does not share its modifiers is worse than
    // one that is not Win-based.
    //
    // These are new value names rather than the old ArrangeTile*/
    // ArrangeCascade* ones, with no fallback to them on purpose: the
    // stored values there are 'T' and 'C', which are the wrong keys for
    // these commands, so inheriting them would silently leave a user on a
    // hotkey that no longer matches anything the menu says.
    UINT arrangeTwoWayHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT arrangeTwoWayHotkeyVirtualKey = '2';
    UINT arrangeThreeWayHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT arrangeThreeWayHotkeyVirtualKey = '3';
    UINT arrangeFourWayHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT arrangeFourWayHotkeyVirtualKey = '4';

    // The "New Stack" global hotkey -- RegisterHotKey's own modifier
    // flags (MOD_ALT/MOD_CONTROL/MOD_SHIFT/MOD_WIN, OR'd together) and
    // a single virtual-key code. Default Ctrl+Alt+1: the tiling commands are
    // Ctrl+Alt+2/3/4, so the whole family sits together, and the digit is
    // the number you would tile by -- stacks are the "one place, many
    // windows" end of it. User-configurable (see StackHotkeyDialog) because
    // a machine can already have a combination claimed by something else.
    //
    // It used to be Win+Alt+G, which was already claimed on the dev machine
    // (RegisterHotKey failed with error 1409), so the shipped default never
    // worked there. LoadSettings treats a stored Win+Alt+G as "never
    // changed" and moves it to the new default; see there.
    UINT stackHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT stackHotkeyVirtualKey = '1';
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

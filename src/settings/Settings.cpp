#include "settings/Settings.h"

#include <string>

namespace polish {

namespace {

constexpr wchar_t kSettingsKeyPath[] = L"Software\\Polish";
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"Polish";

DWORD ReadDword(HKEY key, const wchar_t* valueName, DWORD defaultValue) {
    DWORD value = defaultValue;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_DWORD) {
        return defaultValue;
    }
    return value;
}

void WriteDword(HKEY key, const wchar_t* valueName, DWORD value) {
    RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

}  // namespace

Settings LoadSettings() {
    Settings settings;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKeyPath, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        settings.restoreSyncEnabled = ReadDword(key, L"RestoreSyncEnabled", 1) != 0;
        settings.altTabEnabled = ReadDword(key, L"AltTabEnabled", 1) != 0;
        settings.haloEnabled = ReadDword(key, L"HaloEnabled", 1) != 0;
        settings.bullseyeEnabled = ReadDword(key, L"BullseyeEnabled", 1) != 0;
        settings.taskbarEnabled = ReadDword(key, L"TaskbarEnabled", 1) != 0;
        settings.moveModeEnabled = ReadDword(key, L"MoveModeEnabled", 1) != 0;
        settings.moveModeSnapThresholdPx = ReadDword(key, L"MoveModeSnapThresholdPx", 12);
        settings.arrangeTwoWayHotkeyModifiers =
            ReadDword(key, L"ArrangeTwoWayHotkeyModifiers", MOD_CONTROL | MOD_ALT);
        settings.arrangeTwoWayHotkeyVirtualKey = ReadDword(key, L"ArrangeTwoWayHotkeyVirtualKey", '2');
        settings.arrangeThreeWayHotkeyModifiers =
            ReadDword(key, L"ArrangeThreeWayHotkeyModifiers", MOD_CONTROL | MOD_ALT);
        settings.arrangeThreeWayHotkeyVirtualKey = ReadDword(key, L"ArrangeThreeWayHotkeyVirtualKey", '3');
        settings.arrangeFourWayHotkeyModifiers =
            ReadDword(key, L"ArrangeFourWayHotkeyModifiers", MOD_CONTROL | MOD_ALT);
        settings.arrangeFourWayHotkeyVirtualKey = ReadDword(key, L"ArrangeFourWayHotkeyVirtualKey", '4');
        // The stack hotkey used to be the group hotkey, stored under the old
        // names. Read those as the default, so a combination the user chose
        // before the rename survives it; the new names win once saved.
        settings.stackHotkeyModifiers = ReadDword(
            key, L"StackHotkeyModifiers", ReadDword(key, L"GroupHotkeyModifiers", MOD_CONTROL | MOD_ALT));
        settings.stackHotkeyVirtualKey =
            ReadDword(key, L"StackHotkeyVirtualKey", ReadDword(key, L"GroupHotkeyVirtualKey", '1'));
        // Win+Alt+G was the old shipped default, and SaveSettings writes the
        // current value whenever *any* setting is saved -- so a user who never
        // touched this hotkey still has it stored, and would be stuck on a
        // combination that does not even register on some machines. Nothing
        // distinguishes "left at the default" from "chose it", so a stored
        // Win+Alt+G is treated as the former and moved to the new default.
        if (settings.stackHotkeyModifiers == (MOD_WIN | MOD_ALT) && settings.stackHotkeyVirtualKey == 'G') {
            settings.stackHotkeyModifiers = MOD_CONTROL | MOD_ALT;
            settings.stackHotkeyVirtualKey = '1';
        }
        RegCloseKey(key);
    }
    // If the key doesn't exist yet (first run), settings keeps its
    // all-enabled defaults -- nothing to do.
    return settings;
}

void SaveSettings(const Settings& settings) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    WriteDword(key, L"RestoreSyncEnabled", settings.restoreSyncEnabled ? 1 : 0);
    WriteDword(key, L"AltTabEnabled", settings.altTabEnabled ? 1 : 0);
    WriteDword(key, L"HaloEnabled", settings.haloEnabled ? 1 : 0);
    WriteDword(key, L"BullseyeEnabled", settings.bullseyeEnabled ? 1 : 0);
    WriteDword(key, L"TaskbarEnabled", settings.taskbarEnabled ? 1 : 0);
    WriteDword(key, L"MoveModeEnabled", settings.moveModeEnabled ? 1 : 0);
    WriteDword(key, L"MoveModeSnapThresholdPx", settings.moveModeSnapThresholdPx);
    WriteDword(key, L"ArrangeTwoWayHotkeyModifiers", settings.arrangeTwoWayHotkeyModifiers);
    WriteDword(key, L"ArrangeTwoWayHotkeyVirtualKey", settings.arrangeTwoWayHotkeyVirtualKey);
    WriteDword(key, L"ArrangeThreeWayHotkeyModifiers", settings.arrangeThreeWayHotkeyModifiers);
    WriteDword(key, L"ArrangeThreeWayHotkeyVirtualKey", settings.arrangeThreeWayHotkeyVirtualKey);
    WriteDword(key, L"ArrangeFourWayHotkeyModifiers", settings.arrangeFourWayHotkeyModifiers);
    WriteDword(key, L"ArrangeFourWayHotkeyVirtualKey", settings.arrangeFourWayHotkeyVirtualKey);
    WriteDword(key, L"StackHotkeyModifiers", settings.stackHotkeyModifiers);
    WriteDword(key, L"StackHotkeyVirtualKey", settings.stackHotkeyVirtualKey);
    RegCloseKey(key);
}

bool IsStartAtLoginEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[MAX_PATH]{};
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool exists = RegQueryValueExW(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(value),
                                          &size) == ERROR_SUCCESS &&
                         type == REG_SZ;
    RegCloseKey(key);
    return exists;
}

void SetStartAtLoginEnabled(bool enabled) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    if (enabled) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        const std::wstring quoted = L"\"" + std::wstring(exePath) + L"\"";
        RegSetValueExW(key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted.c_str()),
                        static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValueName);
    }
    RegCloseKey(key);
}

}  // namespace polish

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
        settings.groupHotkeyModifiers = ReadDword(key, L"GroupHotkeyModifiers", MOD_WIN | MOD_ALT);
        settings.groupHotkeyVirtualKey = ReadDword(key, L"GroupHotkeyVirtualKey", 'G');
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
    WriteDword(key, L"GroupHotkeyModifiers", settings.groupHotkeyModifiers);
    WriteDword(key, L"GroupHotkeyVirtualKey", settings.groupHotkeyVirtualKey);
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

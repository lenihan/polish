#include "util/TaskbarState.h"

#include <shellapi.h>

namespace polish {

TaskbarAlignment ReadTaskbarAlignment() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return TaskbarAlignment::Center;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    const bool ok = RegQueryValueExW(key, L"TaskbarAl", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
                        ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    return ok && value == 0 ? TaskbarAlignment::Left : TaskbarAlignment::Center;
}

bool IsTaskbarAutoHide() {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    return (SHAppBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE) != 0;
}

}  // namespace polish

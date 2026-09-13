#include "util/WindowIcon.h"

#include <shellapi.h>

#include <iterator>

namespace polish {

namespace {

// Packaged/UWP-hosted apps (Settings, Store, ...) often run their real
// UI inside a frame window that doesn't answer WM_GETICON or carry a
// meaningful class icon -- but the shell always knows an icon for the
// process's own executable, the same one Explorer/the taskbar would
// show for it. Returns nullptr on any failure (e.g. no permission to
// query the process) rather than throwing -- this is a last-resort
// fallback, not a load-bearing path.
HICON GetProcessExecutableIcon(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return nullptr;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return nullptr;
    }
    wchar_t path[MAX_PATH];
    DWORD pathLength = static_cast<DWORD>(std::size(path));
    const bool gotPath = QueryFullProcessImageNameW(process, 0, path, &pathLength) != FALSE;
    CloseHandle(process);
    if (!gotPath) {
        return nullptr;
    }
    SHFILEINFOW fileInfo{};
    if (SHGetFileInfoW(path, 0, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_SMALLICON) == 0) {
        return nullptr;
    }
    // Caller-owned (unlike every other path in GetWindowIconHandle) --
    // deliberately leaked rather than destroyed here or pushed onto the
    // caller; see GetWindowIconHandle's own header comment for why.
    return fileInfo.hIcon;
}

}  // namespace

HICON GetWindowIconHandle(HWND hwnd) {
    HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL2, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    if (icon == nullptr) {
        icon = GetProcessExecutableIcon(hwnd);
    }
    return icon;
}

}  // namespace polish

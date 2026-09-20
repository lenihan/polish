#include "util/AppIdentity.h"

#include <appmodel.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winver.h>

#include <cwchar>
#include <iterator>
#include <vector>

namespace polish {

namespace {

// The window class of a packaged app's own top-level window, hosted
// inside its ApplicationFrameWindow. The same class name
// GroupPickerWindow::LogCandidates already calls out when dumping
// candidates.
constexpr wchar_t kCoreWindowClassName[] = L"Windows.UI.Core.CoreWindow";

BOOL CALLBACK FindCoreWindowChildProc(HWND hwnd, LPARAM lParam) {
    wchar_t className[64];
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) > 0 &&
        wcscmp(className, kCoreWindowClassName) == 0) {
        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE;  // found it -- stop enumerating
    }
    return TRUE;
}

std::optional<std::wstring> AumidForProcess(HANDLE process) {
    UINT32 length = 0;
    if (GetApplicationUserModelId(process, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return std::nullopt;
    }
    std::wstring aumid(length, L'\0');
    if (GetApplicationUserModelId(process, &length, aumid.data()) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    aumid.resize(length == 0 ? 0 : length - 1);  // drop the trailing null
    return aumid;
}

std::optional<std::wstring> ImagePathForProcess(HANDLE process) {
    // MAX_PATH is not a real ceiling on NT paths, but every other caller
    // in this codebase (and Explorer's own icon lookup) lives with it, and
    // a longer path simply resolves to "unknown" rather than misbehaving.
    wchar_t path[MAX_PATH];
    DWORD length = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &length) == FALSE) {
        return std::nullopt;
    }
    return std::wstring(path, length);
}

DWORD ProcessIdOfWindow(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    return processId;
}

}  // namespace

HWND FindCoreWindowChild(HWND hwnd) {
    HWND coreWindow = nullptr;
    EnumChildWindows(hwnd, FindCoreWindowChildProc, reinterpret_cast<LPARAM>(&coreWindow));
    return coreWindow;
}

std::optional<std::wstring> GetPackagedAppAumid(HWND hwnd) {
    // Asking the frame window's own process would get
    // ApplicationFrameHost.exe, which is shared by every packaged app and
    // has no app identity of its own -- the CoreWindow child is what
    // belongs to the actual app.
    const HWND appWindow = FindCoreWindowChild(hwnd);
    const HWND target = appWindow != nullptr ? appWindow : hwnd;

    const DWORD processId = ProcessIdOfWindow(target);
    if (processId == 0) {
        return std::nullopt;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return std::nullopt;
    }
    std::optional<std::wstring> aumid = AumidForProcess(process);
    CloseHandle(process);
    return aumid;
}

std::optional<std::wstring> GetWindowProcessImagePath(HWND hwnd) {
    const DWORD processId = ProcessIdOfWindow(hwnd);
    if (processId == 0) {
        return std::nullopt;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return std::nullopt;
    }
    std::optional<std::wstring> path = ImagePathForProcess(process);
    CloseHandle(process);
    return path;
}


}  // namespace polish

#include "windowtracking/WindowFilters.h"

namespace polish {

bool IsCandidateWindowShape(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }
    if (!IsWindowVisible(hwnd)) {
        return false;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0) {
        return false;
    }
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == 0) {
        return false;
    }
    // Excludes a real, confirmed case: a hidden/suspended UWP host process
    // (ApplicationFrameHost.exe -- Settings and other first-party UWP
    // apps often stay resident even when the user believes they're
    // closed) can leave a second, title-less ApplicationFrameWindow
    // sitting right alongside the real, titled one, otherwise passing
    // every check above -- confirmed live via `hwnd:"title"[class]`
    // candidate-dump logging, which caught it as a bare
    // `0x...:""[ApplicationFrameWindow]` entry next to the real
    // `"Settings"` one. Alt+Tab landing on that phantom instead of the
    // real window promotes/borders/commits to something with no visible
    // content of its own -- indistinguishable from "the real window
    // stayed hidden behind whatever was in front" from the user's point
    // of view. A real, user-facing window always has some title text;
    // nothing legitimate is excluded by requiring one.
    if (GetWindowTextLengthW(hwnd) == 0) {
        return false;
    }
    return true;
}

bool IsCandidateWindow(HWND hwnd) { return IsCandidateWindowShape(hwnd) && !IsIconic(hwnd); }

bool IsMinimizedCandidateWindow(HWND hwnd) { return IsCandidateWindowShape(hwnd) && IsIconic(hwnd); }

bool IsElevatedWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        // Couldn't even open it at our privilege level -- most likely
        // explained by it being more privileged than us. Exclude
        // defensively rather than assuming otherwise.
        return true;
    }

    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

}  // namespace polish

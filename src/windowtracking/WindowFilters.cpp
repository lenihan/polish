#include "windowtracking/WindowFilters.h"

#include <dwmapi.h>

#include <cstring>
#include <iterator>

#include "util/AppIdentity.h"

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
    // WS_CAPTION *or* WS_THICKFRAME, not WS_CAPTION alone.
    //
    // Requiring a caption excludes real windows, confirmed live: a
    // Copilot window (Chrome_WidgetWin_1, titled "Copilot", 2760x1580,
    // unowned, uncloaked, visible) carries style 0x160F0000 --
    // WS_SYSMENU, WS_THICKFRAME and both min/max boxes, but no
    // WS_CAPTION, because it draws its own title bar. Its two sibling
    // Copilot windows *do* have WS_CAPTION, so this is not "that app is
    // special", it is "the same app in a different window mode". The
    // taskbar counted all three; Polish saw two, which showed up as a
    // hover panel headed "3 running windows" listing two, and as that
    // window being silently unreachable by Alt+Tab.
    //
    // WS_THICKFRAME is the useful stand-in: a resizable top-level window
    // is a real one whether or not the OS draws its frame. It is not a
    // loosening on its own either -- everything here still has to be
    // visible, unowned, not a tool window, titled and uncloaked, which is
    // what actually keeps the junk out (see the comments below).
    if ((style & (WS_CAPTION | WS_THICKFRAME)) == 0) {
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
    // A suspended/cloaked UWP window keeps WS_VISIBLE, WS_CAPTION, no
    // owner and its real title ("Settings") -- probed live: the only
    // window on this desktop passing every check above with a non-zero
    // DWMWA_CLOAKED. The title-length guard above was added for the
    // *title-less* twin of this same app; the titled, cloaked one needs
    // this. Any non-zero cloak value is excluded, not just
    // DWM_CLOAKED_SHELL: other-virtual-desktop windows are cloaked the
    // same way, and leaving those out of Alt+Tab matches Windows' own
    // current-desktop-only Alt+Tab default.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
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

bool IsUnreparentableWindow(HWND hwnd) {
    wchar_t className[64];
    return GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) > 0 &&
           wcscmp(className, kApplicationFrameWindowClass) == 0;
}

}  // namespace polish

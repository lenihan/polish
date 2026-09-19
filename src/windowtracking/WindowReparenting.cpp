#include "windowtracking/WindowReparenting.h"

#include <dwmapi.h>

#include <format>

#include "util/Logging.h"

namespace polish {

namespace {
// Frame-related style bits stripped on reparent -- the group's own
// chrome provides the title bar/tab strip context now, so a member
// showing its own title bar/resize border/system menu inside the
// content area would just be visual clutter, not a second way to
// control the window (GroupManager owns its position/size entirely
// once it's a member).
constexpr LONG_PTR kFrameStyleBits =
    WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_POPUP;

// hwnd:"title"[class], the same shape main.cpp's Alt+Tab candidate dump
// and GroupPickerWindow::LogCandidates use -- the class name is what
// makes a UWP host frame (ApplicationFrameWindow) recognizable in the
// log, which is exactly the case this path has trouble with.
std::wstring DescribeWindow(HWND hwnd) {
    wchar_t title[128] = L"";
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    wchar_t className[128] = L"";
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    return std::format(L"{}:\"{}\"[{}]", reinterpret_cast<void*>(hwnd), title, className);
}

// SWP_FRAMECHANGED forces Windows to recompute the non-client area from
// the current style -- a plain SetWindowLongPtr doesn't take visual
// effect on its own until something triggers that recalculation.
void ApplyFrameChange(HWND hwnd) {
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
}  // namespace

std::optional<ReparentBackup> ReparentIntoGroup(HWND hwnd, HWND newParent) {
    ReparentBackup backup;
    backup.style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    const LONG_PTR newStyle = (backup.style & ~kFrameStyleBits) | WS_CHILD;
    SetWindowLongPtrW(hwnd, GWL_STYLE, newStyle);

    // SetParent between two windows with different DPI_AWARENESS_CONTEXTs
    // fails unless the calling thread first opts into mixed hosting.
    // Polish is Per-Monitor-V2 (see app.manifest), and a UWP app's
    // ApplicationFrameWindow -- which hosts a CoreWindow belonging to a
    // *different* process, with its own awareness -- is precisely the
    // mismatched case this opt-in exists for. Scoped to just this call
    // and restored immediately: it's a thread-wide setting, and nothing
    // else in this app wants mixed-DPI hosting semantics.
    const DPI_HOSTING_BEHAVIOR previousHosting = SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED);
    // SetParent returns the *previous* parent, and a top-level window's
    // previous parent is legitimately nullptr -- so the return value
    // alone can't distinguish success from failure here. Clearing the
    // last error first and checking it after is the documented way to
    // tell them apart.
    SetLastError(ERROR_SUCCESS);
    const HWND previousParent = SetParent(hwnd, newParent);
    const DWORD parentError = GetLastError();
    SetThreadDpiHostingBehavior(previousHosting);

    if (previousParent == nullptr && parentError != ERROR_SUCCESS) {
        // Put the window back exactly as it was found. Without this the
        // caller would be handed a "member" that is WS_CHILD of the
        // desktop: alive, listed, and permanently invisible.
        SetWindowLongPtrW(hwnd, GWL_STYLE, backup.style);
        ApplyFrameChange(hwnd);
        LogDebug(std::format(L"[Polish] Reparent: FAILED for {} -- SetParent error {}, window left top-level",
                              DescribeWindow(hwnd), parentError));
        return std::nullopt;
    }

    ApplyFrameChange(hwnd);

    // A UWP frame can be cloaked by process lifetime management while
    // still reporting WS_VISIBLE and a perfectly valid rect -- the same
    // "visible but not actually rendering" state WindowFilters already
    // screens for *before* a window joins a group, but which nothing
    // re-checked afterward. Logged rather than acted on: if a member
    // joins successfully and still draws nothing, this line is what
    // distinguishes "cloaked/suspended" from "SetParent silently did
    // something else".
    int cloaked = 0;
    const bool cloakKnown =
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)));
    LogDebug(std::format(L"[Polish] Reparent: {} into {} -- ok, cloaked={}", DescribeWindow(hwnd),
                          reinterpret_cast<void*>(newParent), cloakKnown ? std::to_wstring(cloaked) : L"?"));
    return backup;
}

bool ReapplyChildFrameStyles(HWND hwnd) {
    const LONG_PTR currentStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR strippedStyle = currentStyle & ~kFrameStyleBits;
    if (strippedStyle == currentStyle) {
        return false;  // nothing to strip -- the common, every-reflow case
    }
    SetWindowLongPtrW(hwnd, GWL_STYLE, strippedStyle);
    ApplyFrameChange(hwnd);
    LogDebug(std::format(L"[Polish] Group: re-stripped frame styles for {} -- app re-applied its own frame "
                          L"(was 0x{:x}, now 0x{:x})",
                          DescribeWindow(hwnd), static_cast<unsigned long long>(currentStyle),
                          static_cast<unsigned long long>(strippedStyle)));
    return true;
}

void RestoreTopLevel(HWND hwnd, const ReparentBackup& backup) {
    SetWindowLongPtrW(hwnd, GWL_STYLE, backup.style);
    // Same mixed-hosting opt-in as ReparentIntoGroup, for the same
    // reason -- releasing a member is the identical cross-awareness
    // SetParent call in the other direction, and a member that failed to
    // return to top-level would be a far worse bug than one that failed
    // to join (an app the user can no longer reach at all).
    const DPI_HOSTING_BEHAVIOR previousHosting = SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED);
    SetLastError(ERROR_SUCCESS);
    SetParent(hwnd, nullptr);
    const DWORD parentError = GetLastError();
    SetThreadDpiHostingBehavior(previousHosting);
    if (parentError != ERROR_SUCCESS) {
        LogDebug(std::format(L"[Polish] Reparent: release FAILED for {} -- SetParent error {}",
                              DescribeWindow(hwnd), parentError));
    }
    ApplyFrameChange(hwnd);
}

}  // namespace polish

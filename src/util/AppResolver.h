#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace polish {

// Resolves the AppUserModelID the *shell itself* stacks a window under --
// the same string the taskbar puts in its button's UI Automation
// AutomationId (which spells it with a literal "Appid: " prefix). Matching
// on this is what makes "which windows does this taskbar button stand
// for?" exact rather than a heuristic.
//
// Wrapped in a class rather than exposed as a free function because the
// underlying COM object is worth creating once and reusing: resolving one
// taskbar button means asking about every candidate window in turn, and
// paying a CoCreateInstance per window would be wasteful. Owned as a
// single instance by main.cpp, the same way the UIA worker is, so it is
// destroyed before the process calls CoUninitialize.
//
// Apartment-affine: create and use it on one thread only (the main STA
// thread here). Every call is a bounded, in-process COM call -- no UI, no
// cross-process UIA -- but it is still nowhere near cheap enough for a
// low-level hook callback, so results belong in a cache.
class AppResolver {
public:
    AppResolver();
    ~AppResolver();

    AppResolver(const AppResolver&) = delete;
    AppResolver& operator=(const AppResolver&) = delete;

    // False when the resolver could not be created at all, in which case
    // AppIdForWindow always returns nullopt and the caller should disable
    // whatever feature depends on it rather than fall back to guessing.
    bool IsAvailable() const;

    // The shell's AppUserModelID for hwnd, or nullopt.
    //
    // std::nullopt is a real, expected answer, not only an error: fails
    // closed so a caller declines to act rather than acting on the wrong
    // window. A window that resolves to nothing simply matches no taskbar
    // button.
    //
    // Measured on Win11 26200 before this was written, because the
    // documented approach does not actually work. SHGetPropertyStoreForWindow
    // with PKEY_AppUserModel_ID -- the route every reference recommends --
    // returned *nothing* for most ordinary Win32 windows (VS Code, Outlook,
    // OneNote and Terminal all came back empty), because that property only
    // carries an AUMID a window explicitly set on itself; packaged apps and
    // Edge were the only ones that answered. IApplicationResolver is what
    // the shell uses to derive one otherwise, and it answered for every
    // window tested, each matching its own taskbar button exactly --
    // including VS Code resolving to two windows against a button that read
    // "2 running windows".
    //
    // IApplicationResolver is undocumented, but it has been present since
    // Windows 7 and the shell consults it for every taskbar stacking
    // decision. The explicit window property is still tried first as a
    // cheap fast path when a window does set one.
    //
    // Deliberately no executable-path fallback. A path could never equal
    // the AUMID a taskbar button reports, so it would not help match a
    // window to a button -- it would only invent a stacking key of our own
    // that disagrees with the shell's.
    std::optional<std::wstring> AppIdForWindow(HWND hwnd) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Strips the literal "Appid: " prefix the taskbar's UIA AutomationId
// carries, leaving the bare AppUserModelID that AppIdForWindow returns.
// Returns the input unchanged when the prefix is absent, so an OS build
// that stops prefixing degrades to still working rather than to matching
// nothing.
//
// Pure, so it is unit-tested -- see tests/TaskbarButtonsTests.cpp.
std::wstring StripAppIdPrefix(std::wstring_view automationId);

}  // namespace polish

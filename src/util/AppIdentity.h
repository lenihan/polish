#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace polish {

// A UWP/Store app's real top-level window is hosted as a child of its
// ApplicationFrameWindow but belongs to a *different* process (the app's
// own -- Calculator.exe, say -- not the shared ApplicationFrameHost.exe
// that owns the frame). nullptr if hwnd has no such child, i.e. for any
// ordinary Win32 window.
HWND FindCoreWindowChild(HWND hwnd);

// The Application User Model ID of the packaged app behind `hwnd` -- the
// identity the shell keys everything about a Store app on (its taskbar
// button, its icon in AppsFolder, its app view). Handles being given
// either the ApplicationFrameWindow (it finds the CoreWindow child
// itself) or the app's own window directly.
//
// std::nullopt for anything that isn't a packaged app, which is the
// common, expected, silent case -- GetApplicationUserModelId returns
// APPMODEL_ERROR_NO_APPLICATION for a plain Win32 process.
std::optional<std::wstring> GetPackagedAppAumid(HWND hwnd);

}  // namespace polish

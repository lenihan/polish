#pragma once

#include <windows.h>

namespace polish {

// The small icon a window itself advertises via WM_GETICON (falling
// back to the window class's icon, then to the large icon if no small
// one exists) -- the same lookup order Explorer/the taskbar use. Falls
// back further still, if all of those come back empty, to extracting a
// shell icon from the window's owning process's own executable
// (SHGetFileInfoW) -- needed for packaged/UWP-hosted apps (Settings,
// Store, ...) whose real top-level frame window often doesn't answer
// WM_GETICON or carry a class icon at all, unlike a classic Win32 app.
//
// Every caller may treat the returned handle as borrowed and never
// destroy it: the WM_GETICON/class-icon paths already return handles
// owned by the window/class, and the shell-icon fallback path
// deliberately leaks its own (SHGetFileInfoW-allocated, caller-owned)
// icon rather than pushing a mixed ownership contract onto every
// existing call site -- acceptable for the handful of dialog-lifetime
// icons this is ever called for, not a pattern to reuse somewhere
// long-running or called at scale.
HICON GetWindowIconHandle(HWND hwnd);

}  // namespace polish

#pragma once

#include <windows.h>

namespace polish {

// The small icon a window itself advertises via WM_GETICON (falling
// back to the window class's icon, then to the large icon if no small
// one exists) -- the same lookup order Explorer/the taskbar use.
// Returned handles are borrowed from their owning window/class; never
// destroy them.
HICON GetWindowIconHandle(HWND hwnd);

}  // namespace polish

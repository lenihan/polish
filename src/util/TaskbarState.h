#pragma once

#include <windows.h>

#include "windowtracking/TaskbarPlacement.h"

namespace polish {

// How the taskbar is arranged, read from the system. Cheap enough to ask
// on every placement (a registry value and one shell message), so nothing
// here is cached: the user can change either in Settings while Polish runs.

// Start-button alignment. HKCU\...\Explorer\Advanced\TaskbarAl: 0 is left,
// 1 -- or absent, the default -- is centered.
TaskbarAlignment ReadTaskbarAlignment();

// Whether the taskbar is set to hide itself. SHAppBarMessage(ABM_GETSTATE).
bool IsTaskbarAutoHide();

}  // namespace polish

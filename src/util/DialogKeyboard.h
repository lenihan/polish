#pragma once

#include <windows.h>

#include <cstddef>

namespace polish {

// Moves focus to the next (or previous) enabled stop in `stops`,
// wrapping. The shared implementation behind every hand-rolled Tab ring
// in this app.
//
// Written by hand rather than deferred to the OS because none of this
// app's "dialogs" are real Win32 dialogs: they're custom window classes
// with their own modal GetMessage loops, so there is no dialog manager,
// no IsDialogMessage, and no WS_TABSTOP machinery to lean on -- and
// several stops are custom-painted child windows that dialog navigation
// could not drive even if there were.
//
// `stops` is in the dialog's own reading order. Null entries are skipped
// (a control that doesn't exist in this configuration), and so are
// disabled ones: SetFocus on a disabled window is a silent no-op, which
// would otherwise leave Tab stuck on whatever stop preceded it
// (confirmed live, with the picker's Create button disabled on an empty
// group). Bounded to one full lap, so an all-disabled ring terminates
// instead of spinning.
//
// A focus that isn't in `stops` at all (or no focus yet) is treated as
// index 0, so the first Tab press always has a well-defined destination.
void CycleFocus(const HWND* stops, size_t count, bool backward);

// Whether `msg` is a key-down aimed at `dialog` or one of its children.
// The guard every modal pump in this app needs before acting on a key:
// the pump sees every message for the thread, including ones belonging
// to other windows entirely.
bool IsDialogKeyDown(const MSG& msg, HWND dialog, UINT virtualKey);

}  // namespace polish

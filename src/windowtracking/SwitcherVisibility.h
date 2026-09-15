#pragma once

#include <windows.h>

namespace polish {

// Hides (or restores) a window's entry in the shell's own switchers --
// the taskbar button and Windows' native Alt+Tab.
//
// This exists because every *documented* lever fails for a packaged
// (UWP/Store) app's window, all verified live against Calculator:
// giving it an owner is refused outright (GWLP_HWNDPARENT, error 87),
// WS_EX_TOOLWINDOW is accepted and read back as set but ignored by the
// taskbar, and ITaskbarList::DeleteTab returns success and changes
// nothing -- with or without the hide/show cycle that normally makes the
// shell re-evaluate. A packaged app's taskbar button is bound to its
// package identity via ApplicationFrameHost, not to the HWND.
//
// What does work is the same internal interface the virtual-desktop
// feature itself uses: IApplicationView::SetShowInSwitchers, reached
// through the ImmersiveShell service. That interface is **undocumented**
// and its vtable layout has shifted between Windows feature updates, so
// calling the wrong slot would mean calling an arbitrary function with
// the wrong arguments. Two guards make that safe rather than reckless:
//
//  1. Before relying on the layout, this calls GetAppUserModelId (an
//     earlier slot in the same vtable) and checks the string it returns
//     against the AUMID resolved independently from the app's own
//     process (see util/AppIdentity.h). If those don't match, the
//     layout isn't what this code was written against and nothing
//     further is called.
//  2. The calls themselves run under SEH, so a layout mismatch severe
//     enough to fault is caught and disables the feature for the rest
//     of the session instead of taking the app down.
//
// Either guard tripping is logged and leaves the window exactly as it
// was -- the window keeps its taskbar button, which is the same
// behavior as not having tried. Returns whether the change was actually
// applied, so a caller can log/record the difference rather than
// assume.
bool SetWindowShownInSwitchers(HWND hwnd, bool shown);

}  // namespace polish

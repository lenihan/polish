#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace polish {

// One app button on one taskbar, as read from UI Automation.
//
// Deliberately holds no HWND: a Windows 11 taskbar button has none
// (NativeWindowHandle comes back 0), which is also why it can neither be
// invoked through UIA nor sent a message. Identity is the AppUserModelID
// and position is the rect, and those two are all any caller needs.
struct TaskbarButton {
    // The bare AppUserModelID, with the AutomationId's "Appid: " prefix
    // already stripped -- see StripAppIdPrefix in util/AppResolver.h.
    std::wstring appId;
    // Screen rect in physical pixels, straight from UIA's
    // BoundingRectangle. Physical, not DIPs: MSLLHOOKSTRUCT::pt is
    // physical too, so a hook-thread hit-test compares like with like and
    // needs no scaling.
    //
    // This holds only while the process stays Per-Monitor-V2 aware (which
    // app.manifest declares and main.cpp asserts at startup). Confirmed
    // the hard way against a test harness that had no manifest: UIA
    // virtualizes BoundingRectangle for a DPI-unaware process, and every
    // rect came back at exactly half size on this 200% display --
    // self-consistent, and wrong by a factor of two against the real
    // cursor position. A hit-test would then silently match the wrong
    // button, or none.
    RECT rect{};
    // The button's UIA Name, exactly as Windows writes it -- "Visual
    // Studio Code - 2 running windows", and localized. Carried so the
    // hover panel can head its list with whatever the OS itself calls
    // the app, rather than the AppUserModelID, which is a key and not a
    // name ("Microsoft.VisualStudioCode", "MSEdge").
    //
    // Deliberately used whole, running-window count and all, instead of
    // being trimmed back to just the app name: the separator is a plain
    // hyphen that app names are free to contain, and the suffix differs
    // per language, so any trim would be a guess that reads as correct
    // on the machine it was written on. The full string is also simply
    // more useful as a heading.
    std::wstring name;
    // Which taskbar this came from -- the primary Shell_TrayWnd or one of
    // the per-monitor Shell_SecondaryTrayWnd bars. Carried because
    // AutomationId values repeat across taskbars: the same app pinned on
    // two monitors yields two buttons with the identical appId, so appId
    // alone does not identify a button.
    HMONITOR taskbar = nullptr;
};

// The button containing `screenPoint`, or nullopt.
//
// Pure and allocation-free by design: this is the one thing the low-level
// mouse hook is allowed to call on every mouse event, and a low-level hook
// that does not return promptly is silently and undetectably unhooked by
// Windows (LowLevelHooksTimeout). Everything expensive -- reading the
// buttons out of UIA, resolving which windows an app owns -- happens off
// the hook thread and lands in the vector passed here.
//
// Buttons never overlap in practice, so on the pathological case of
// overlapping rects the first match wins, arbitrarily but predictably.
std::optional<TaskbarButton> HitTestTaskbarButton(const std::vector<TaskbarButton>& buttons, POINT screenPoint);

// Whether two button snapshots describe the same taskbar state. Used to
// tell "the taskbar actually changed, rebuild what depends on it" apart
// from "a periodic refresh returned the same thing", so a safety-net
// re-enumeration does not churn anything downstream.
//
// Names are compared along with ids and rects, which makes a window
// opening or closing within one app count as a change even when the
// strip has not moved a pixel -- the count lives in the name. That is
// the wanted answer: anything downstream holding a list of that app's
// windows is stale the moment one appears or disappears.
bool TaskbarButtonsEqual(const std::vector<TaskbarButton>& a, const std::vector<TaskbarButton>& b);

}  // namespace polish

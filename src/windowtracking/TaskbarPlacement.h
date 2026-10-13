#pragma once

#include <windows.h>

#include <optional>
#include <vector>

namespace polish {

// Where Polish's own taskbar button goes. Pure rect math, Win32-free past
// RECT, so the cases that matter -- a centered taskbar re-centering as apps
// open, a left-aligned one, a read that found no Start button -- are
// settled by tests and not by launching apps and looking.
//
// Windows 11 has no API for a third-party taskbar button, so the button is
// a Polish-owned window drawn over free taskbar space (see
// PolishTaskbarButton). That makes "free" the whole problem: the native
// buttons are packed edge to edge, and anything that overlaps one steals
// its clicks.

enum class TaskbarAlignment {
    // The default: Start and the app buttons form one group, centered.
    Center,
    // Start at the left edge, apps following it.
    Left,
};

struct TaskbarGeometry {
    // The taskbar's own screen rect, physical pixels.
    RECT taskbar{};
    // The Start button, when the read found it.
    std::optional<RECT> start;
    // Every app button on this taskbar, in any order.
    std::vector<RECT> appButtons;
    TaskbarAlignment alignment = TaskbarAlignment::Center;
    // For the fallback size only; the button otherwise takes Start's.
    UINT dpi = 96;
};

// The button's rect, or nullopt when there is nowhere sensible to put it
// (the caller hides the button).
//
// - Size: Start's own size, so it reads as a sibling of Start. Without a
//   Start rect, 44 x 48 DIP at the given DPI -- Windows 11's button cell.
// - Centered: immediately left of Start, with no gap, the way Task View
//   sits beside Search. The group re-centers whenever an app opens or
//   closes, so this moves with it; the caller re-reads on every such
//   change.
// - Left-aligned: Start is the leftmost thing, so there is no room beside
//   it on its left. The button goes just after the last app button
//   instead, which keeps it next to the group; with no apps, after Start.
// - Never past the left edge of the taskbar, and never into the rightmost
//   quarter, which is the system tray.
std::optional<RECT> PolishButtonRect(const TaskbarGeometry& geometry);

}  // namespace polish

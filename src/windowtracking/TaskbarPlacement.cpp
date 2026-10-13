#include "windowtracking/TaskbarPlacement.h"

#include <algorithm>

namespace polish {

namespace {

constexpr int kCellWidthDip = 44;
constexpr int kCellHeightDip = 48;

}  // namespace

std::optional<RECT> PolishButtonRect(const TaskbarGeometry& geometry) {
    const RECT& taskbar = geometry.taskbar;
    const LONG taskbarWidth = taskbar.right - taskbar.left;
    const LONG taskbarHeight = taskbar.bottom - taskbar.top;
    if (taskbarWidth <= 0 || taskbarHeight <= 0) {
        return std::nullopt;
    }

    // Size and vertical extent: Start's when known, so the button sits in
    // the same row as the native ones.
    LONG width = MulDiv(kCellWidthDip, static_cast<int>(geometry.dpi), 96);
    LONG top = taskbar.top;
    LONG bottom = taskbar.bottom;
    if (geometry.start.has_value() && geometry.start->right > geometry.start->left) {
        width = geometry.start->right - geometry.start->left;
        top = geometry.start->top;
        bottom = geometry.start->bottom;
    } else {
        const LONG height = std::min<LONG>(MulDiv(kCellHeightDip, static_cast<int>(geometry.dpi), 96), taskbarHeight);
        top = taskbar.top + (taskbarHeight - height) / 2;
        bottom = top + height;
    }

    LONG left = 0;
    if (geometry.alignment == TaskbarAlignment::Center) {
        if (geometry.start.has_value()) {
            left = geometry.start->left - width;
        } else if (!geometry.appButtons.empty()) {
            // No Start rect: the group's own left edge is the next best
            // anchor, and Start is one cell to the left of it or so.
            LONG first = geometry.appButtons.front().left;
            for (const RECT& button : geometry.appButtons) {
                first = std::min(first, button.left);
            }
            left = first - width;
        } else {
            left = taskbar.left + taskbarWidth / 2 - width;
        }
    } else {
        // Left-aligned: after the last app button, or after Start if there
        // are none.
        LONG anchor = geometry.start.has_value() ? geometry.start->right : taskbar.left;
        for (const RECT& button : geometry.appButtons) {
            anchor = std::max(anchor, button.right);
        }
        left = anchor;
    }

    const RECT result{left, top, left + width, bottom};
    if (result.left < taskbar.left) {
        return std::nullopt;
    }
    // The rightmost quarter is the tray, clock and overflow: never
    // overlapped, however many apps are open.
    const LONG trayStart = taskbar.right - taskbarWidth / 4;
    if (result.right > trayStart) {
        return std::nullopt;
    }
    return result;
}

}  // namespace polish

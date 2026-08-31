#pragma once

#include <windows.h>

#include <vector>

namespace polish {

// Tracks window activation order (most-recently-used first), independent
// of any notion of whether a window is currently a valid/visible/
// non-minimized candidate for anything -- that filtering happens where
// this list is consumed (e.g. building an Alt+Tab candidate list),
// keeping this class pure and trivially testable, matching the same
// "small testable class, Win32-heavy logic lives elsewhere" split
// RectHistory/RectUtils used before they were deleted.
class ActivationHistory {
public:
    // Moves hwnd to the front of the order, inserting it if not already
    // present. Call on every window activation (e.g. EVENT_SYSTEM_FOREGROUND).
    void MoveToFront(HWND hwnd);

    // Removes hwnd from the order, if present. Call on window destruction
    // (e.g. EVENT_OBJECT_DESTROY) so stale handles don't accumulate.
    void Remove(HWND hwnd);

    // Most-recently-activated first.
    std::vector<HWND> OrderedWindows() const;

private:
    std::vector<HWND> order_;
};

}  // namespace polish

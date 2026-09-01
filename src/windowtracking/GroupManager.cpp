#include "windowtracking/GroupManager.h"

#include <algorithm>
#include <cmath>

#include "windowtracking/WindowZOrder.h"

namespace polish {

GroupId GroupManager::CreateGroup(const std::vector<HWND>& windows, GroupMode mode) {
    const GroupId id = nextId_++;
    GroupState state(id, mode);
    for (HWND hwnd : windows) {
        state.AddWindow(hwnd);
    }
    groups_.push_back(std::move(state));
    return id;
}

GroupState* GroupManager::FindGroup(GroupId id) {
    auto it = std::find_if(groups_.begin(), groups_.end(), [id](const GroupState& g) { return g.Id() == id; });
    return it == groups_.end() ? nullptr : &*it;
}

GroupState* GroupManager::FindGroupContaining(HWND hwnd) {
    auto it = std::find_if(groups_.begin(), groups_.end(), [hwnd](const GroupState& g) { return g.Contains(hwnd); });
    return it == groups_.end() ? nullptr : &*it;
}

namespace {
// Restores hwnd first if it's still maximized -- SetWindowPos silently
// no-ops on size/position otherwise (confirmed M0 finding) -- then
// positions it into `rect`. Returns hwnd's *actual* resulting rect,
// which can be larger than requested: SetWindowPos silently clamps to a
// window's own declared minimum tracking size rather than failing --
// confirmed with both Notepad (small default chrome) and Outlook (a
// real user report) -- so a caller that only trusts the requested rect
// would leave that member visibly overflowing the group.
RECT RestoreIfMaximizedAndPosition(HWND hwnd, const RECT& rect) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    RECT actual{};
    GetWindowRect(hwnd, &actual);
    return actual;
}
}  // namespace

SIZE GroupManager::ApplyLayout(const GroupState& group, const RECT& contentRect) {
    if (group.Mode() == GroupMode::Tile) {
        return ApplyTileLayout(group, contentRect);
    }
    return ApplyTabLayout(group, contentRect);
}

SIZE GroupManager::ApplyTabLayout(const GroupState& group, const RECT& contentRect) {
    const int requestedWidth = contentRect.right - contentRect.left;
    const int requestedHeight = contentRect.bottom - contentRect.top;
    int neededWidth = requestedWidth;
    int neededHeight = requestedHeight;

    for (const GroupMember& member : group.Members()) {
        if (member.kind != GroupMemberKind::Window || member.window == nullptr || !IsWindow(member.window)) {
            continue;  // nested-group case -- v1 never populates this
        }
        const RECT actual = RestoreIfMaximizedAndPosition(member.window, contentRect);
        neededWidth = std::max(neededWidth, static_cast<int>(actual.right - actual.left));
        neededHeight = std::max(neededHeight, static_cast<int>(actual.bottom - actual.top));
    }

    if (const std::optional<HWND> active = group.ActiveWindow(); active.has_value() && IsWindow(*active)) {
        PromoteWindowToFront(*active);
    }

    return SIZE{neededWidth, neededHeight};
}

SIZE GroupManager::ApplyTileLayout(const GroupState& group, const RECT& contentRect) {
    // Real (non-nested-group) members only -- see the Tab-layout loop's
    // same filter. Counted separately from group.Members().size() so
    // the grid isn't sized larger than what will actually get a slot.
    std::vector<HWND> windows;
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr && IsWindow(member.window)) {
            windows.push_back(member.window);
        }
    }
    const int requestedWidth = contentRect.right - contentRect.left;
    const int requestedHeight = contentRect.bottom - contentRect.top;
    if (windows.empty()) {
        return SIZE{requestedWidth, requestedHeight};
    }

    const int count = static_cast<int>(windows.size());
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + cols - 1) / cols;

    // Per-column/row required size, like an HTML table's auto layout --
    // a single oversized member (its own minimum size bigger than its
    // slot) only grows its own column/row, not the whole grid uniformly.
    std::vector<int> colWidths(static_cast<size_t>(cols), 0);
    std::vector<int> rowHeights(static_cast<size_t>(rows), 0);

    for (int i = 0; i < count; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        // Slot edges computed from contentRect.left/top plus the *next*
        // column/row boundary, not left + slotWidth per cell -- avoids
        // integer-division remainder pixels accumulating into a visible
        // gap or overlap at the grid's right/bottom edge.
        const RECT slot{contentRect.left + col * requestedWidth / cols, contentRect.top + row * requestedHeight / rows,
                         contentRect.left + (col + 1) * requestedWidth / cols,
                         contentRect.top + (row + 1) * requestedHeight / rows};
        const RECT actual = RestoreIfMaximizedAndPosition(windows[static_cast<size_t>(i)], slot);
        colWidths[static_cast<size_t>(col)] =
            std::max(colWidths[static_cast<size_t>(col)], static_cast<int>(actual.right - actual.left));
        rowHeights[static_cast<size_t>(row)] =
            std::max(rowHeights[static_cast<size_t>(row)], static_cast<int>(actual.bottom - actual.top));
    }

    int neededWidth = 0;
    for (int w : colWidths) {
        neededWidth += w;
    }
    int neededHeight = 0;
    for (int h : rowHeights) {
        neededHeight += h;
    }

    return SIZE{std::max(neededWidth, requestedWidth), std::max(neededHeight, requestedHeight)};
}

}  // namespace polish

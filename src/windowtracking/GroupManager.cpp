#include "windowtracking/GroupManager.h"

#include <algorithm>
#include <cmath>

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

void GroupManager::EnsureReparented(HWND hwnd, HWND chromeWindow) {
    if (reparentBackups_.contains(hwnd)) {
        return;  // already a child of some group chrome
    }
    reparentBackups_[hwnd] = ReparentIntoGroup(hwnd, chromeWindow);
}

void GroupManager::ReleaseGroup(const GroupState& group) {
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr) {
            ReleaseMember(member.window);
        }
    }
}

void GroupManager::ReleaseMember(HWND hwnd) {
    const auto it = reparentBackups_.find(hwnd);
    if (it == reparentBackups_.end()) {
        return;
    }
    if (IsWindow(hwnd)) {
        RestoreTopLevel(hwnd, it->second);
    }
    reparentBackups_.erase(it);
}

namespace {
// Restores hwnd first if it's still maximized (SetWindowPos silently
// no-ops on size/position otherwise -- confirmed M0 finding), then
// positions it into `rect` (client-area-relative coordinates, since
// hwnd is a child window now) and shows/hides it. Returns hwnd's
// *actual* resulting rect in screen coordinates (GetWindowRect always
// reports screen coordinates regardless of parent/child status) --
// which can be larger than requested: SetWindowPos silently clamps to
// a window's own declared minimum tracking size rather than failing --
// confirmed with both Notepad (small default chrome) and Outlook (a
// real user report) -- so a caller that only trusts the requested rect
// would leave that member visibly overflowing the group.
RECT PositionMember(HWND hwnd, const RECT& rect, bool visible) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (visible) {
        // SWP_SHOWWINDOW makes it visible but doesn't guarantee its
        // content actually repaints -- confirmed real: switching tabs
        // showed a blank window until the user moved the mouse over it.
        // A window that was just hidden (or freshly reparented) can sit
        // on a stale/uncomposited DWM redirection surface until
        // something forces it to redraw; RDW_ALLCHILDREN covers apps
        // like Explorer that are themselves made of child panes.
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
    }
    RECT actual{};
    GetWindowRect(hwnd, &actual);
    return actual;
}
}  // namespace

SIZE GroupManager::ApplyLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRectClientCoords) {
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr && IsWindow(member.window)) {
            EnsureReparented(member.window, chromeWindow);
        }
    }

    if (group.Mode() == GroupMode::Tile) {
        return ApplyTileLayout(group, chromeWindow, contentRectClientCoords);
    }
    return ApplyTabLayout(group, chromeWindow, contentRectClientCoords);
}

SIZE GroupManager::ApplyTabLayout(const GroupState& group, HWND /*chromeWindow*/, const RECT& contentRect) {
    const int requestedWidth = contentRect.right - contentRect.left;
    const int requestedHeight = contentRect.bottom - contentRect.top;
    int neededWidth = requestedWidth;
    int neededHeight = requestedHeight;

    // Two passes, not one: every *inactive* member is hidden first,
    // and the active member is shown (and redrawn -- see
    // PositionMember) strictly last. In member-list order within a
    // single pass, switching back to an earlier member in the list
    // would show+redraw it before a later member's hide ran -- a real,
    // confirmed case of that later hide visibly undoing the earlier
    // member's redraw (switching to tab 2 worked; switching back to
    // tab 1 showed blank). Doing every hide before the one show
    // guarantees the show+redraw is never followed by anything else
    // touching the group's content area.
    const std::optional<HWND> active = group.ActiveWindow();
    for (const GroupMember& member : group.Members()) {
        if (member.kind != GroupMemberKind::Window || member.window == nullptr || !IsWindow(member.window)) {
            continue;  // nested-group case -- v1 never populates this
        }
        const bool isActive = active.has_value() && member.window == *active;
        if (isActive) {
            continue;  // handled after this loop, once every hide is done
        }
        const RECT actualRect = PositionMember(member.window, contentRect, false);
        neededWidth = std::max(neededWidth, static_cast<int>(actualRect.right - actualRect.left));
        neededHeight = std::max(neededHeight, static_cast<int>(actualRect.bottom - actualRect.top));
    }
    if (active.has_value() && IsWindow(*active)) {
        const RECT actualRect = PositionMember(*active, contentRect, true);
        neededWidth = std::max(neededWidth, static_cast<int>(actualRect.right - actualRect.left));
        neededHeight = std::max(neededHeight, static_cast<int>(actualRect.bottom - actualRect.top));
    }

    return SIZE{neededWidth, neededHeight};
}

SIZE GroupManager::ApplyTileLayout(const GroupState& group, HWND /*chromeWindow*/, const RECT& contentRect) {
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
        const RECT actual = PositionMember(windows[static_cast<size_t>(i)], slot, true);
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

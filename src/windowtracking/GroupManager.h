#pragma once

#include <windows.h>

#include <vector>

#include "windowtracking/GroupState.h"

namespace polish {

// Owns every group Polish currently knows about, and the M4 logic that
// actually positions/promotes member windows -- unlike GroupState
// itself, this is not pure/Win32-free (ApplyLayout drives real
// SetWindowPos calls), so it isn't unit tested the same exhaustive way;
// CreateGroup/FindGroup's bookkeeping is still covered by
// GroupManagerTests.cpp.
class GroupManager {
public:
    // Creates a new group containing exactly `windows`, in the given
    // order, and returns its id. An empty `windows` list is allowed (a
    // group with no members yet).
    GroupId CreateGroup(const std::vector<HWND>& windows, GroupMode mode = GroupMode::Tab);

    size_t GroupCount() const { return groups_.size(); }
    const std::vector<GroupState>& Groups() const { return groups_; }

    // nullptr if no group with this id exists.
    GroupState* FindGroup(GroupId id);

    // nullptr if hwnd isn't a member of any group Polish knows about
    // (never matches a group's own chrome window -- chrome is never
    // added as a member of its own group).
    GroupState* FindGroupContaining(HWND hwnd);

    // Positions every member of `group` into `contentRect` (screen
    // coordinates), restoring any still-maximized member first
    // (SetWindowPos silently no-ops on size/position otherwise -- a
    // confirmed M0 finding). Behavior depends on group.Mode():
    //  - Tab: every member occupies the identical rect, and the active
    //    member is brought to front via the shared promote/demote pulse
    //    (WindowZOrder::PromoteWindowToFront) -- every other member is
    //    left fully live and WS_VISIBLE, just pushed behind in z-order,
    //    never minimized/restyled (PLAN.md's Alt+Tab-integration note).
    //    Since every member shares one rect, this same call handles both
    //    an initial layout and a group-drag's continuous re-drive: the
    //    rect is just wherever the chrome's content area currently is.
    //  - Tile: contentRect is divided into a roughly square grid (one
    //    slot per member), and every member is positioned into its own
    //    slot -- all simultaneously visible, so there's no z-order
    //    promotion to do (they don't overlap).
    //
    // Returns the smallest content-area size that would fit every
    // member without any of them being clamped by their own declared
    // minimum window size (confirmed real -- e.g. Outlook refuses to
    // shrink below a size larger than a freshly-created group's default
    // chrome, and SetWindowPos silently clamps to it instead of
    // failing). Never smaller than contentRect's own size. Equal to
    // contentRect's size when every member fit as requested -- callers
    // should compare and, if it's larger, grow the chrome/contentRect to
    // at least this size and call ApplyLayout again, rather than leaving
    // a member visibly overflowing the group.
    SIZE ApplyLayout(const GroupState& group, const RECT& contentRect);

private:
    SIZE ApplyTabLayout(const GroupState& group, const RECT& contentRect);
    SIZE ApplyTileLayout(const GroupState& group, const RECT& contentRect);

    std::vector<GroupState> groups_;
    GroupId nextId_ = 1;
};

}  // namespace polish

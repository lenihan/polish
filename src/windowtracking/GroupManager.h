#pragma once

#include <windows.h>

#include <map>
#include <vector>

#include "windowtracking/GroupState.h"
#include "windowtracking/WindowReparenting.h"

namespace polish {

// Owns every group Polish currently knows about, and the logic that
// actually reparents/positions/shows member windows -- unlike
// GroupState itself, this is not pure/Win32-free, so it isn't unit
// tested the same exhaustive way; CreateGroup/FindGroup's bookkeeping
// is still covered by GroupManagerTests.cpp.
//
// Members are real children (WS_CHILD) of the group's chrome window --
// true containment, not just repositioning -- so ApplyLayout needs the
// chrome's HWND, and ReleaseGroup/ReleaseMember must be called before a
// member/chrome is ever destroyed independently of the other (see
// WindowReparenting.h's comment on why: a child destroyed along with
// its parent silently loses whatever wasn't saved).
class GroupManager {
public:
    ~GroupManager();

    // Creates a new group containing exactly `windows`, in the given
    // order, and returns its id. An empty `windows` list is allowed (a
    // group with no members yet). Does not reparent anything itself --
    // that happens the first time ApplyLayout runs for this group, once
    // its chrome window exists.
    GroupId CreateGroup(const std::vector<HWND>& windows, GroupMode mode = GroupMode::Tab);

    size_t GroupCount() const { return groups_.size(); }
    const std::vector<GroupState>& Groups() const { return groups_; }

    // nullptr if no group with this id exists.
    GroupState* FindGroup(GroupId id);

    // nullptr if hwnd isn't a member of any group Polish knows about
    // (never matches a group's own chrome window -- chrome is never
    // added as a member of its own group).
    GroupState* FindGroupContaining(HWND hwnd);

    // Reparents any not-yet-reparented member into `chromeWindow` (see
    // the class comment), restores any still-maximized member first
    // (SetWindowPos silently no-ops on size/position otherwise -- a
    // confirmed M0 finding), then positions every member into
    // `contentRectClientCoords` -- *client-area-relative* coordinates
    // (a child window's SetWindowPos x/y are relative to its parent's
    // client origin, not the screen), unlike the old reposition-only
    // design's screen coordinates. Behavior depends on group.Mode():
    //  - Tab: every member occupies the identical rect; only the active
    //    one is shown (SW_SHOW), every other is hidden (SW_HIDE) -- not
    //    minimized, just not visible, so switching tabs is instant with
    //    no z-order ambiguity now that they're true siblings under the
    //    same parent (the old promote/demote HWND_TOPMOST pulse was for
    //    independent top-level windows and doesn't apply to children).
    //  - Tile: contentRectClientCoords is divided into a roughly square
    //    grid (one slot per member), and every member is positioned
    //    into its own slot and shown simultaneously.
    //
    // Returns the smallest content-area size that would fit every
    // member without any of them being clamped by their own declared
    // minimum window size (confirmed real -- e.g. Outlook refuses to
    // shrink below a size larger than a freshly-created group's default
    // chrome, and SetWindowPos silently clamps to it instead of
    // failing). Never smaller than contentRectClientCoords's own size.
    // Equal to it when every member fit as requested -- callers should
    // compare and, if it's larger, grow the chrome to at least this
    // size and call ApplyLayout again, rather than leaving a member
    // visibly overflowing the group.
    SIZE ApplyLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRectClientCoords);

    // Restores every reparented member of `group` back to an
    // independent top-level window (WindowReparenting::RestoreTopLevel)
    // -- must be called before the group's chrome window is destroyed
    // (app exit, or an explicit "close group"), or every member still
    // parented to it would be destroyed along with it.
    void ReleaseGroup(const GroupState& group);

    // Restores a single member back to top-level -- used when removing
    // one window from a group (edit-membership) without disbanding the
    // whole group. No-op if hwnd was never reparented by this manager
    // (e.g. it was never actually added, or already released).
    void ReleaseMember(HWND hwnd);

    // A static preview of a Tab-mode member's content, captured the
    // instant it was last hidden (switched away from) -- used for the
    // tab hover-preview popup. nullptr if hwnd has never been hidden as
    // a member yet (e.g. it's the currently-active one, or a brand-new
    // group whose first reflow hasn't run). Capturing happens
    // automatically inside ApplyLayout -- see ApplyTabLayout's own
    // comment for why it must happen at that exact moment rather than
    // lazily whenever a caller wants to show a preview.
    HBITMAP CachedThumbnail(HWND hwnd) const;

private:
    SIZE ApplyTabLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRect);
    SIZE ApplyTileLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRect);
    void EnsureReparented(HWND hwnd, HWND chromeWindow);
    void CaptureThumbnail(HWND hwnd);

    std::vector<GroupState> groups_;
    GroupId nextId_ = 1;
    // Only contains entries for currently-reparented members -- absence
    // means "not a child of any group chrome right now," which
    // EnsureReparented uses to decide whether reparenting is still
    // needed.
    std::map<HWND, ReparentBackup> reparentBackups_;
    // Owned by this class -- freed on ReleaseMember and in the
    // destructor. See CachedThumbnail.
    std::map<HWND, HBITMAP> memberThumbnails_;
};

}  // namespace polish

#pragma once

#include <windows.h>

#include <map>
#include <vector>

#include "windowtracking/GroupState.h"
#include "windowtracking/WindowReparenting.h"

namespace polish {

// The tile grid's column/row count for `count` simultaneously-visible
// members. Tile: biased wide (cols >= rows, Horizontal) or tall
// (rows >= cols, Vertical) -- ceil(sqrt(count)) in the biased dimension,
// however many of the other dimension that leaves. Stack: forced to a
// single row (Horizontal) or single column (Vertical) regardless of
// count -- the "third layout mode" between Tab and full Tile. A free
// function (not a GroupManager member) so it's unit-testable without a
// live GroupManager/HWNDs -- everything downstream of the shape
// (fractions, splitter gaps, positioning) still lives in
// GroupManager::ApplyTileLayout, which calls this for the shape itself.
struct GridShape {
    int cols;
    int rows;
};
GridShape ComputeGridShape(GroupMode mode, GroupAlignment alignment, int count);

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
    //  - Tile/Stack: contentRectClientCoords is divided into a grid (see
    //    ComputeGridShape -- Tile roughly square, Stack a single row/
    //    column), user-resizable via TileColumnBoundaries/
    //    TileRowBoundaries/SetTileBoundary, and every member is
    //    positioned into its own slot and shown simultaneously.
    //    `tileSplitterWidthPx` (ignored in Tab mode) reserves that many
    //    real pixels between adjacent columns/rows for the chrome's
    //    draggable splitter -- members never overlap it, unlike an
    //    earlier version of this where the splitter was drawn *over*
    //    the members' shared edge and could visibly race with their
    //    own repaints.
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
    SIZE ApplyLayout(GroupState& group, HWND chromeWindow, const RECT& contentRectClientCoords,
                      int tileSplitterWidthPx = 0);

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

    // Re-captures hwnd's thumbnail if it's currently hidden (a no-op,
    // returning false, for a visible/active member or a window that's
    // gone). Returns true if the new capture looks different from
    // whatever was cached before (a cheap sampled-pixel comparison, not
    // exact) -- there's no universal Win32 signal for "this window has
    // finished loading its own content" (every app manages that
    // internally without exposing it), so a caller wanting real
    // confidence the capture is stable should call this repeatedly a
    // few times a short interval apart and stop once it returns false
    // twice in a row, rather than trusting a single capture or a fixed
    // delay. Used both for a delayed background sweep after a reflow,
    // and for a short live "settle" loop while a tab is actively being
    // hovered (see main.cpp's kThumbnailRefreshTimerId and
    // kThumbnailStabilizeTimerId).
    bool RefreshThumbnail(HWND hwnd);

    // Tile/Stack only: the last-computed column/row boundaries from the
    // most recent ApplyLayout call -- content-rect-relative pixel
    // positions, *excluding* the two outer edges (N columns have N-1
    // of these), each the *center* of that column/row pair's reserved
    // splitter gap (see ApplyLayout's own comment on
    // tileSplitterWidthPx). Empty if the group is in Tab mode, has 0-1
    // members, or ApplyLayout hasn't run for it yet. Used by the chrome
    // to render/hit-test the resize splitters between tiles.
    std::vector<int> TileColumnBoundaries(GroupId id) const;
    std::vector<int> TileRowBoundaries(GroupId id) const;

    // Tile/Stack only: moves the boundary between column (or row, if
    // `column` is false) `index` and `index + 1` to `newPixelPosition`
    // -- content-rect-relative, in the *same* real/gap-reserved pixel
    // space as TileColumnBoundaries/TileRowBoundaries (a splitter gap
    // center), not the content-only space ApplyLayout's fractions
    // divide up internally; this converts between the two itself.
    // `totalSize` is the content area's current real width (columns)
    // or height (rows); `splitterWidthPx` must match whatever was
    // passed to the ApplyLayout call that produced the boundaries
    // being dragged. Only the two adjacent cells' stored fractions
    // change, every other column/row is untouched, matching standard
    // splitter behavior. Clamps within the pair's own combined span as
    // a safety net; the primary minimum-visible-size enforcement
    // happens in pixel space in the chrome, before this is even called
    // (see GroupChromeWindow's own splitter-drag handling). No-op if
    // `index + 1` is out of range for the group's current column/row
    // count.
    void SetTileBoundary(GroupState& group, bool column, size_t index, int newPixelPosition, int totalSize,
                          int splitterWidthPx);

private:
    SIZE ApplyTabLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRect);
    SIZE ApplyTileLayout(GroupState& group, HWND chromeWindow, const RECT& contentRect, int splitterWidthPx);
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
    // Populated by ApplyTileLayout each time it runs. See
    // TileColumnBoundaries/TileRowBoundaries.
    std::map<GroupId, std::vector<int>> tileColumnBoundaries_;
    std::map<GroupId, std::vector<int>> tileRowBoundaries_;
};

}  // namespace polish

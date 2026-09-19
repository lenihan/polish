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
// A member joins by becoming a real child (WS_CHILD) of the group's
// chrome window (see EnsureReparented) -- true containment, can't be
// dragged outside the chrome's client area. A window that categorically
// cannot be reparented this way (a UWP frame window, confirmed live:
// SetParent fails outright for ApplicationFrameWindow every time) is
// refused outright rather than joined some other way -- see
// WindowFilters::IsUnreparentableWindow, which the group picker uses to
// grey such a window out up front.
//
// ApplyLayout needs the chrome's HWND, and ReleaseGroup/ReleaseMember
// must be called before a member/chrome is ever destroyed independently
// of the other (see WindowReparenting.h's comments on why: a child
// destroyed along with its parent silently loses whatever wasn't
// saved).
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

    // Reparents any not-yet-joined member into `chromeWindow` (see
    // EnsureReparented), dropping (GroupState::Remove) any member that
    // turns out to be unreparentable -- rather than leaving it in place
    // to fail the identical attempt again on every future call. Callers
    // that care about membership changing out from under them (to
    // resync a chrome's own tab labels, say) should compare
    // group.MemberCount() before and after. Restores any still-maximized
    // member first (SetWindowPos silently no-ops on size/position
    // otherwise -- a confirmed M0 finding), then positions every member
    // into
    // `contentRectClientCoords` -- *client-area-relative* coordinates
    // (a child window's SetWindowPos x/y are relative to its parent's
    // client origin, not the screen) -- unlike the old reposition-only
    // design's screen coordinates. Behavior depends on group.Mode():
    //  - Tab: every member occupies the identical rect and stays shown;
    //    only the active one is Z-ordered on top (see ApplyTabLayout's
    //    own comment for why nothing is ever hidden).
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

    // Restores every member of `group` back to an independent top-level
    // window (WindowReparenting::RestoreTopLevel) -- must be called
    // before the group's chrome window is destroyed (app exit, or an
    // explicit "close group"), or every member still parented by it
    // would be destroyed along with it.
    void ReleaseGroup(const GroupState& group);

    // Restores a single member back to independent top-level -- used
    // when removing one window from a group (edit-membership) without
    // disbanding the whole group. No-op if hwnd was never joined by this
    // manager (e.g. it was never actually added, or already released).
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

    // Puts hwnd back into the rect the last ApplyLayout pass positioned
    // it into, if it has since drifted -- a user dragging or resizing a
    // member inside the group (confirmed real: Tab mode's member fills
    // the whole content area but nothing stopped it being resized
    // smaller, revealing the members Z-ordered behind it). A member's
    // geometry is owned entirely by this class; nothing else is allowed
    // to move one. No-op for a window that isn't a currently laid-out
    // member (memberRects_ has no entry for it), or one already where it
    // belongs, or one whose correction has been suspended for refusing
    // to stay put more than kMaxCorrectionsPerWindow (see
    // correctionWindowStart_/correctionCount_) -- cleared the next time
    // ApplyLayout runs for that member's group. Returns whether it moved
    // anything, for callers that only want to log a correction when one
    // actually happened.
    bool EnforceMemberRect(HWND hwnd);

private:
    SIZE ApplyTabLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRect);
    SIZE ApplyTileLayout(GroupState& group, HWND chromeWindow, const RECT& contentRect, int splitterWidthPx);
    // Tries to embed hwnd (ReparentIntoGroup). Returns false if hwnd is
    // a known-unreparentable window (WindowFilters::IsUnreparentableWindow)
    // or ReparentIntoGroup itself fails, which ApplyLayout uses to drop a
    // member that can never join, rather than retrying it every reflow
    // forever. A window already joined (present in reparentBackups_) is
    // a no-op returning true.
    bool EnsureReparented(HWND hwnd, HWND chromeWindow);
    void CaptureThumbnail(HWND hwnd);
    // How many device px of `member`'s own leading edge (top in
    // Horizontal alignment, left in Vertical) it claims for its own
    // resize hit-testing -- confirmed live that some custom-frame apps
    // (File Explorer) do this entirely on their own, independent of
    // WS_THICKFRAME and without ever deferring WM_SETCURSOR to this
    // app's chrome. A member with a nonzero band here gets that band
    // covered by an opaque overlay window (see PlaceResizeBandOverlay) --
    // the mouse lands on the overlay instead of the member, so the
    // member's own frame never gets asked and never shows its resize
    // cursor there. Lazily measured (WM_NCHITTEST probing -- see its own
    // .cpp comment) and cached per member/axis, since it's a property of
    // the app's own frame, not of the current layout pass; a member
    // practically never changes this after joining. Cleared in
    // ReleaseMember and ApplyLayout's unjoinable-drop loop, same as
    // memberRects_.
    int ResizeBandPx(HWND member, bool vertical);
    // Appends to `out` the rect(s) of `member`'s own resize band(s) that
    // need covering, in `slot`'s own coordinate space (chrome-client --
    // same space `slot` itself is already in). Always checks the *top*
    // edge --
    // that's where the band actually reported by the app lives
    // (TITLE_BAR_SCAFFOLDING_WINDOW_CLASS, confirmed live) regardless of
    // this group's alignment. When `vertical` is true, also checks the
    // *left* edge, since Vertical alignment puts the tab strip/header
    // there instead, and that's the edge a member's own frame is
    // actually adjacent to and might also claim a band along (unverified
    // whether any real app does; checking costs one more cheap
    // already-cached lookup either way). A member with no band on either
    // edge appends nothing.
    void AppendResizeBandRects(HWND member, const RECT& slot, bool vertical, std::vector<RECT>& out);
    // Self-check that `member`'s top resize band (see ResizeBandPx) is
    // actually covered, rather than trusting that placing an overlay
    // worked -- exists so a coverage gap shows up in the log on its own,
    // without anyone having to hover the exact spot by hand (this bug
    // class has already taken several rounds of "still see it" reports
    // to pin down once). `slot` is `member`'s own occupied rect in
    // chrome-client coordinates (only ever called for an embedded
    // member); `chrome` is used to convert the handful of sample points
    // to screen coordinates. Samples the top ~24px at 10%, 50% and 90%
    // across the slot's width: for each point, finds the deepest window
    // in `member`'s own subtree and asks its WM_NCHITTEST answer (same
    // technique as MeasureLeadingResizeBand); if that answer is a
    // top-edge resize code, checks which window is actually topmost on
    // screen there (WindowFromPoint) -- if that isn't this group's own
    // resize-band overlay, logs one line and stops (at most one line per
    // call, so a real gap doesn't flood the log the way an earlier,
    // unrelated bug already did once).
    void LogUncoveredResizeBands(HWND chrome, HWND member, const RECT& slot);
    // Positions overlay `index` of `chrome`'s pool over `rect` (chrome
    // client coordinates), creating it if the pool doesn't reach that far
    // yet, and raises it above every member. See
    // kResizeBandOverlayClassName's own comment for what these are for.
    void PlaceResizeBandOverlay(HWND chrome, size_t index, const RECT& rect);
    // Hides every overlay in `chrome`'s pool from `usedCount` on -- the
    // ones the layout pass that just ran didn't need (a member left the
    // group, a mode switch needs fewer, ...). Hidden rather than
    // destroyed so the next pass can reuse them without paying
    // CreateWindowExW again.
    void HideUnusedResizeBandOverlays(HWND chrome, size_t usedCount);

    std::vector<GroupState> groups_;
    GroupId nextId_ = 1;
    // Absence means "not a child of any group chrome right now," which
    // EnsureReparented uses to decide whether reparenting is still
    // needed.
    std::map<HWND, ReparentBackup> reparentBackups_;
    // Owned by this class -- freed on ReleaseMember and in the
    // destructor. See CachedThumbnail.
    std::map<HWND, HBITMAP> memberThumbnails_;
    // The rect the most recent ApplyLayout pass positioned each member
    // into, in chrome-*client*-relative coordinates (the same space
    // PositionMember's own `rect` argument uses -- SetWindowPos for a
    // child window is relative to its parent's client origin). Populated
    // at every PositionMember call site, erased in ReleaseMember and
    // ApplyLayout's unjoinable-drop loop -- absence means "not a member
    // this class is currently positioning," same "map membership is the
    // source of truth" shape reparentBackups_ already uses.
    std::map<HWND, RECT> memberRects_;
    // See ResizeBandPx. Two maps, one per axis -- a member practically
    // never needs both during its lifetime (alignment rarely changes on
    // an already-joined group), so there's no real cost to keeping the
    // stale one around if it ever does.
    std::map<HWND, int> memberTopResizeBandPx_;
    std::map<HWND, int> memberLeftResizeBandPx_;
    // EnforceMemberRect's own rate limit for its "pulled member back into
    // its slot" log line -- confirmed real that a missing guard once let
    // that line fire 2.3 million times in a single session (a minimized
    // member fighting its own iconic placeholder rect forever, now fixed
    // elsewhere in this function). The correction itself is
    // never skipped; only the log line is throttled. GetTickCount64
    // timestamp of the last time each hwnd's correction was actually
    // logged.
    std::map<HWND, ULONGLONG> lastCorrectionLogTick_;
    // EnforceMemberRect's runaway guard: if a member's correction keeps
    // firing far more often than any real drag could produce, its own
    // frame is fighting the correction (or something else is), and
    // retrying forever just burns CPU pointlessly -- suspend further
    // corrections for that member until the next ApplyLayout call for its
    // group, which clears this. `count`/`windowStart` track how many
    // corrections have fired within the current 1-second window;
    // `suspended` is set once that count is exceeded.
    struct CorrectionThrottle {
        ULONGLONG windowStart = 0;
        int count = 0;
        bool suspended = false;
    };
    std::map<HWND, CorrectionThrottle> correctionThrottles_;
    // Populated by ApplyTileLayout each time it runs. See
    // TileColumnBoundaries/TileRowBoundaries.
    std::map<GroupId, std::vector<int>> tileColumnBoundaries_;
    std::map<GroupId, std::vector<int>> tileRowBoundaries_;
    // Per-chrome pool of resize-band overlay windows (see
    // kResizeBandOverlayClassName's own comment) -- Tab mode needs at
    // most one (only the active member is visible), Tile/Stack one per
    // member whose own frame claims a band. They're children of the
    // chrome, so Windows destroys them along with it; the map entry is
    // dropped in ReleaseGroup.
    std::map<HWND, std::vector<HWND>> resizeBandOverlays_;
};

}  // namespace polish

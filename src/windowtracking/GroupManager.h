#pragma once

#include <windows.h>

#include <shobjidl.h>

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
// A member joins one of two ways, decided automatically per-window (see
// EnsureAttached), never by the user:
//  - Embedded: a real child (WS_CHILD) of the group's chrome window --
//    true containment, can't be dragged outside the chrome's client
//    area. The default, and the only way most (Win32) windows join.
//  - Attached: kept top-level, with the chrome as its *owner*
//    (GWLP_HWNDPARENT) instead of its parent -- the fallback for a
//    window that categorically cannot be reparented (a UWP frame
//    window, confirmed live: SetParent fails outright for
//    ApplicationFrameWindow every time). An owned window stays above
//    its owner and is hidden/minimized/destroyed along with it, without
//    needing WS_CHILD -- so it still moves, minimizes, and closes with
//    the group, just without being visually contained inside it (it
//    keeps its own title bar and isn't clipped to the chrome; see
//    docs/LIMITATIONS.md).
//
// Either way, ApplyLayout needs the chrome's HWND, and ReleaseGroup/
// ReleaseMember must be called before a member/chrome is ever destroyed
// independently of the other (see WindowReparenting.h's comments on
// why: a child destroyed along with its parent silently loses whatever
// wasn't saved, and an *owned* window is destroyed along with its owner
// exactly the same way).
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

    // Attaches/embeds any not-yet-joined member into `chromeWindow` (see
    // the class comment for the embedded/attached split), dropping
    // (GroupState::Remove) any member that turns out to be joinable
    // neither way at all -- see EnsureAttached's own comment -- rather
    // than leaving it in place to fail the identical attempt again on
    // every future call. Callers that care about membership changing
    // out from under them (to resync a chrome's own tab labels, say)
    // should compare group.MemberCount() before and after. Restores any
    // still-maximized member first (SetWindowPos silently no-ops on
    // size/position otherwise -- a confirmed M0 finding), then positions
    // every member into
    // `contentRectClientCoords` -- *client-area-relative* coordinates
    // (a child window's SetWindowPos x/y are relative to its parent's
    // client origin, not the screen; an attached member's slot is
    // converted to screen coordinates internally, see
    // ApplyTabLayout/ApplyTileLayout) -- unlike the old reposition-only
    // design's screen coordinates. Behavior depends on group.Mode():
    //  - Tab: every embedded member occupies the identical rect and
    //    stays shown; only the active one is Z-ordered on top (see
    //    ApplyTabLayout's own comment for why nothing is ever hidden).
    //    An *attached* member can't be covered by Z-order this way (an
    //    owned window is always above its owner) so it's shown only
    //    while active and hidden (SW_HIDE) otherwise.
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

    // Restores every embedded member of `group` back to an independent
    // top-level window (WindowReparenting::RestoreTopLevel) and detaches
    // every attached one (WindowReparenting::DetachFromGroup) -- must be
    // called before the group's chrome window is destroyed (app exit, or
    // an explicit "close group"), or every member still parented/owned
    // by it would be destroyed along with it.
    void ReleaseGroup(const GroupState& group);

    // Restores/detaches a single member back to independent top-level --
    // used when removing one window from a group (edit-membership)
    // without disbanding the whole group. No-op if hwnd was never joined
    // by this manager (e.g. it was never actually added, or already
    // released).
    void ReleaseMember(HWND hwnd);

    // Whether `group` currently has any attached (not embedded) member
    // -- an attached member doesn't move for free with its chrome the
    // way an embedded (WS_CHILD) one does, so a caller repositioning the
    // chrome (main.cpp's onMoved_) needs to know whether it must also
    // reflow the group to drag those along.
    bool HasAttachedMembers(const GroupState& group) const;

    // Re-places whichever attached member(s) of `group` belong directly
    // above `chromeWindow` in Z-order (see GroupManager's own class
    // comment on why this has to be done by hand, and
    // ApplyTabLayout/ApplyTileLayout's own PositionMember calls for the
    // same technique at layout time) -- called whenever the chrome itself
    // is brought forward (GroupChromeWindow::SetOnZOrderChanged) so an
    // attached member, which is not a sibling child and so doesn't ride
    // along with the chrome for free, comes back into its correct
    // position relative to it instead of staying wherever it last was.
    // In Tab mode that's only the *active* attached member -- every other
    // one belongs behind the chrome (covered, same as an embedded
    // sibling), and raising it too would incorrectly surface a covered
    // tab. In Tile/Stack every member is visible in its own slot
    // simultaneously, so every attached one is raised. A no-op for a
    // group with no attached members.
    void RaiseAttachedMembers(const GroupState& group, HWND chromeWindow);

    // Shows or hides every attached member of `group` to match the
    // chrome's own minimized state (GroupChromeWindow::SetOnMinimizedChanged)
    // -- an attached member is a real top-level window, so it doesn't
    // minimize/restore with the chrome the way an embedded (WS_CHILD) one
    // does automatically. `hide` true on minimize; false on restore, in
    // which case the caller should follow with a normal ApplyLayout/
    // ReflowGroupTo pass (not done here) so each member ends up back in
    // its slot with Tab mode's own active-only visibility re-applied,
    // rather than this method blanket-showing every attached member
    // regardless of which tab is active.
    void SetAttachedMembersHidden(const GroupState& group, bool hide);

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
    // belongs. Returns whether it moved anything, for callers that only
    // want to log a correction when one actually happened.
    bool EnforceMemberRect(HWND hwnd);

private:
    SIZE ApplyTabLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRect);
    SIZE ApplyTileLayout(GroupState& group, HWND chromeWindow, const RECT& contentRect, int splitterWidthPx);
    // Tries to embed hwnd (ReparentIntoGroup); if that fails, falls back
    // to attaching it instead (AttachToGroup) -- see the class comment
    // for what each means. Returns false only when *both* fail, which
    // ApplyLayout uses to drop a member that can never join either way,
    // rather than retrying it every reflow forever. A window already
    // joined either way (present in reparentBackups_ or
    // attachedBackups_) is a no-op returning true.
    bool EnsureAttached(HWND hwnd, HWND chromeWindow);
    // Whether hwnd is currently an attached (not embedded) member --
    // i.e. present in attachedBackups_. Membership in exactly one of the
    // two backup maps is the source of truth for which kind a member is.
    bool IsAttached(HWND hwnd) const { return attachedBackups_.contains(hwnd); }
    void CaptureThumbnail(HWND hwnd);
    // How many device px of `member`'s own leading edge (top in
    // Horizontal alignment, left in Vertical) it claims for its own
    // resize hit-testing -- confirmed live that some custom-frame apps
    // (File Explorer) do this entirely on their own, independent of
    // WS_THICKFRAME and without ever deferring WM_SETCURSOR to this
    // app's chrome, so a member with a nonzero band here has its
    // occupied rect shrunk by that much on the leading edge (see
    // ApplyTabLayout/ApplyTileLayout) to keep that band physically
    // outside the member's own window -- the chrome's own content-area
    // background paints through in the reclaimed strip instead (already
    // the active tab's own color, see PaintTabStrip, so this reads as a
    // seamless extension rather than a visible gap). Lazily measured
    // (WM_NCHITTEST probing -- see its own .cpp comment) and cached per
    // member/axis, since it's a property of the app's own frame, not of
    // the current layout pass; a member practically never changes this
    // after joining. Cleared in ReleaseMember and ApplyLayout's
    // unjoinable-drop loop, same as memberRects_.
    int ResizeBandPx(HWND member, bool vertical);
    // Lazily creates (on first use) and returns the shared ITaskbarList
    // instance used to hide/restore an attached member's own taskbar
    // button -- see EnsureAttached/ReleaseMember, the only two callers.
    // An attached member is a plain top-level window with nothing else
    // suppressing its button (no real owner relationship to do it for
    // free -- see this class's own comment), so this is done explicitly
    // via the shell API built for exactly this. Requires COM already
    // initialized on this thread (main.cpp's wWinMain does this once, at
    // startup, before any group operation can run). Returns nullptr (and
    // every call site checks before using it) if CoCreateInstance ever
    // fails -- a missing taskbar button is a cosmetic regression, not a
    // reason to fail joining/leaving a group.
    ITaskbarList* TaskbarListInstance();
    // Keeps an attached member off the taskbar (WS_EX_TOOLWINDOW plus
    // ITaskbarList::DeleteTab -- see the .cpp for why both), and puts it
    // back. Suppression is re-applied on every layout pass, not just at
    // join: the shell re-adds a button for a window it sees being shown,
    // and every layout pass shows this one.
    void SuppressTaskbarButton(HWND hwnd);
    void RestoreTaskbarButton(HWND hwnd);
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
    // Only contains entries for currently-embedded members -- absence
    // means "not a child of any group chrome right now," which
    // EnsureAttached uses to decide whether embedding is still needed.
    std::map<HWND, ReparentBackup> reparentBackups_;
    // The attached-member counterpart of reparentBackups_ above -- same
    // "map membership is the source of truth for current state"
    // contract, just for the other join kind. A window is in at most
    // one of these two maps at a time.
    std::map<HWND, AttachBackup> attachedBackups_;
    // Owned by this class -- freed on ReleaseMember and in the
    // destructor. See CachedThumbnail.
    std::map<HWND, HBITMAP> memberThumbnails_;
    // The rect the most recent ApplyLayout pass positioned each member
    // into, in the member's own natural space: chrome-*client*-relative
    // coordinates for an embedded member (the same space PositionMember's
    // own `rect` argument uses -- SetWindowPos for a child window is
    // relative to its parent's client origin), or *screen* coordinates
    // for an attached one, which isn't a child of anything. Storing each
    // in its own space is deliberate, not an inconsistency: a single
    // screen-coordinate map would go stale for every embedded member the
    // instant the chrome is dragged, since children move with their
    // parent without any layout pass running -- see EnforceMemberRect,
    // which is what actually interprets a stored rect using IsAttached
    // to know which space it's in. Populated at every PositionMember (or
    // attached-equivalent) call site, erased in ReleaseMember and
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
    // Populated by ApplyTileLayout each time it runs. See
    // TileColumnBoundaries/TileRowBoundaries.
    std::map<GroupId, std::vector<int>> tileColumnBoundaries_;
    std::map<GroupId, std::vector<int>> tileRowBoundaries_;
    // See TaskbarListInstance. nullptr until the first attached member
    // actually needs it.
    ITaskbarList* taskbarList_ = nullptr;
    // Per-chrome pool of resize-band overlay windows (see
    // kResizeBandOverlayClassName's own comment) -- Tab mode needs at
    // most one (only the active member is visible), Tile/Stack one per
    // member whose own frame claims a band. They're children of the
    // chrome, so Windows destroys them along with it; the map entry is
    // dropped in ReleaseGroup.
    std::map<HWND, std::vector<HWND>> resizeBandOverlays_;
};

}  // namespace polish

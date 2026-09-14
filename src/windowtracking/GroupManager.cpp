#include "windowtracking/GroupManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include "util/Logging.h"
#include "windowtracking/RectUtils.h"
#include "windowtracking/WindowFilters.h"

namespace polish {

GridShape ComputeGridShape(GroupMode mode, GroupAlignment alignment, int count) {
    if (mode == GroupMode::Stack) {
        // Forced to a single row (Horizontal) or single column
        // (Vertical) -- the "third layout mode" between Tab and full
        // Tile, rather than the biased-square shape below.
        if (alignment == GroupAlignment::Vertical) {
            return GridShape{1, count};
        }
        return GridShape{count, 1};
    }
    // Tile. Horizontal (default): biased wide (cols >= rows) --
    // ceil(sqrt(n)) columns, however many rows that leaves. Vertical:
    // the same formula with columns/rows swapped, biasing tall instead
    // -- for exactly 2 members this is the difference between
    // side-by-side and stacked; for any other count it's the general
    // "grid biased wide vs. tall" behavior GroupState.h's own
    // GroupAlignment comment documents.
    if (alignment == GroupAlignment::Vertical) {
        const int rows = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
        const int cols = (count + rows - 1) / rows;
        return GridShape{cols, rows};
    }
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + cols - 1) / cols;
    return GridShape{cols, rows};
}

namespace {
// PW_RENDERFULLCONTENT (Windows 8.1+) -- captures a window's actual
// rendered content (including hardware-accelerated/DirectComposition
// surfaces a plain BitBlt-based capture can't see), which the plain
// PW_CLIENTONLY-only flag alone doesn't guarantee on every app.
constexpr UINT kPrintWindowRenderFullContent = 0x00000002;

// Restores hwnd if it's currently maximized (SetWindowPos silently
// no-ops on size/position otherwise -- confirmed M0 finding). Shared by
// PositionMember (about to reposition a member) and CaptureThumbnail
// (about to capture one): a member added to a group while still
// maximized was being captured in that state, and PrintWindow returned
// real content only for the window's much smaller *restored*
// footprint, leaving the rest of the (maximized-sized) capture solid
// black -- confirmed via a compiled spike against real File Explorer
// windows, not assumed.
void RestoreIfMaximized(HWND hwnd) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED) {
        ShowWindow(hwnd, SW_RESTORE);
    }
}

// Cheap fingerprint of a bitmap's content (a sampled grid of pixels,
// not every pixel -- this runs several times in a row during the
// stabilize loop, so it needs to stay fast) -- FNV-1a over the sampled
// COLORREFs. Two captures with the same fingerprint are treated as "no
// visible change" by RefreshThumbnail; this is how it detects that a
// member's content has settled without any app-specific signal for
// "I've finished loading."
UINT32 SampleFingerprint(HBITMAP bitmap, int width, int height) {
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);
    UINT32 hash = 2166136261u;  // FNV-1a 32-bit offset basis
    const int stepX = std::max(1, width / 24);
    const int stepY = std::max(1, height / 24);
    for (int y = 0; y < height; y += stepY) {
        for (int x = 0; x < width; x += stepX) {
            const COLORREF c = GetPixel(memDC, x, y);
            hash ^= static_cast<UINT32>(c);
            hash *= 16777619u;  // FNV-1a 32-bit prime
        }
    }
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return hash;
}
}  // namespace

GroupManager::~GroupManager() {
    for (auto& [hwnd, bitmap] : memberThumbnails_) {
        DeleteObject(bitmap);
    }
}

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

bool GroupManager::EnsureAttached(HWND hwnd, HWND chromeWindow) {
    if (reparentBackups_.contains(hwnd) || attachedBackups_.contains(hwnd)) {
        return true;  // already joined, one way or the other
    }
    // Done here, before either join path, not via the existing
    // RestoreIfMaximized calls further down: those all run on a window
    // that is already WS_CHILD, where SW_RESTORE on a WS_MINIMIZE child
    // is not meaningful. Needed now that the picker offers minimized
    // windows as candidates -- ReparentIntoGroup strips the frame bits
    // but not WS_MINIMIZE, so without this a minimized member joins the
    // group as a blank tile.
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    // A known case (a UWP frame window) skips straight to the attach
    // fallback -- SetParent is *guaranteed* to fail for these (confirmed
    // live, ERROR_INVALID_PARAMETER every time), so trying it first would
    // only pay for a style strip and its rollback for no chance of
    // success. Anything else still tries embedding first; attaching is
    // strictly the fallback, since it's the one that doesn't visually
    // contain the member.
    if (!IsUnreparentableWindow(hwnd)) {
        // No backup recorded when the reparent fails -- ReparentIntoGroup
        // has already put the window back the way it found it, so
        // recording one would later hand RestoreTopLevel a window that
        // was never actually reparented, and (worse) make this function
        // believe the window is already a member and skip retrying it on
        // the next layout pass.
        if (std::optional<ReparentBackup> backup = ReparentIntoGroup(hwnd, chromeWindow)) {
            reparentBackups_[hwnd] = *backup;
            return true;
        }
    }
    // Embedding wasn't possible (or wasn't attempted) -- fall back to
    // attaching instead. AttachToGroup always returns a backup (see its
    // own comment), so this path can't fail the way embedding can; a
    // member only ends up dropped from the group entirely if it somehow
    // isn't a valid window at all by the time GetWindow/SetWindowLongPtr
    // run on it.
    attachedBackups_[hwnd] = AttachToGroup(hwnd, chromeWindow);
    return true;
}

void GroupManager::ReleaseGroup(const GroupState& group) {
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr) {
            ReleaseMember(member.window);
        }
    }
}

void GroupManager::ReleaseMember(HWND hwnd) {
    // A window is in at most one of these two maps -- see attachedBackups_'s
    // own comment -- so at most one of these branches ever does anything.
    if (const auto it = reparentBackups_.find(hwnd); it != reparentBackups_.end()) {
        if (IsWindow(hwnd)) {
            RestoreTopLevel(hwnd, it->second);
        }
        reparentBackups_.erase(it);
    } else if (const auto attachedIt = attachedBackups_.find(hwnd); attachedIt != attachedBackups_.end()) {
        if (IsWindow(hwnd)) {
            DetachFromGroup(hwnd, attachedIt->second);
        }
        attachedBackups_.erase(attachedIt);
    } else {
        return;  // never actually a member of this manager's making
    }

    const auto thumbIt = memberThumbnails_.find(hwnd);
    if (thumbIt != memberThumbnails_.end()) {
        DeleteObject(thumbIt->second);
        memberThumbnails_.erase(thumbIt);
    }
    memberRects_.erase(hwnd);
}

bool GroupManager::HasAttachedMembers(const GroupState& group) const {
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr && IsAttached(member.window)) {
            return true;
        }
    }
    return false;
}

bool GroupManager::EnforceMemberRect(HWND hwnd) {
    const auto it = memberRects_.find(hwnd);
    if (it == memberRects_.end() || !IsWindow(hwnd)) {
        return false;
    }
    // memberRects_ is stored in the member's own natural space (see its
    // own comment) -- screen coordinates already for an attached member,
    // but chrome-*client*-relative for an embedded one, so a child's
    // GetWindowRect (always screen coordinates, regardless of parent/
    // child status) needs converting into that same space before
    // comparing -- the same conversion PositionMember's caller logically
    // undoes when it hands *back* a client rect to SetWindowPos.
    RECT current{};
    GetWindowRect(hwnd, &current);
    if (!IsAttached(hwnd)) {
        const HWND parent = GetParent(hwnd);
        if (parent != nullptr) {
            POINT topLeft{current.left, current.top};
            POINT bottomRight{current.right, current.bottom};
            ScreenToClient(parent, &topLeft);
            ScreenToClient(parent, &bottomRight);
            current = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        }
    }
    const RECT& expected = it->second;
    if (RectsApproximatelyEqual(current, expected, 1)) {
        return false;
    }
    // A window whose own declared minimum tracking size is larger than
    // its assigned slot still sits at the right *origin*, just clamped
    // larger by SetWindowPos itself (PositionMember's own comment; the
    // Outlook case) -- ReflowGroupTo's grow-and-reapply path is what
    // handles that, by growing the chrome, not this. Fighting it here
    // (forcing it back to a size it just refused) would just fire a
    // pointless SetWindowPos on every location-change event for that
    // member, so only origin drift and *shrinking* below the assigned
    // size count as something to correct.
    const bool originMatches = std::abs(current.left - expected.left) <= 1 && std::abs(current.top - expected.top) <= 1;
    const bool notSmaller = (current.right - current.left) >= (expected.right - expected.left) - 1 &&
                             (current.bottom - current.top) >= (expected.bottom - expected.top) - 1;
    if (originMatches && notSmaller) {
        return false;
    }
    SetWindowPos(hwnd, nullptr, expected.left, expected.top, expected.right - expected.left,
                 expected.bottom - expected.top, SWP_NOZORDER | SWP_NOACTIVATE);
    LogDebug(std::format(L"[Polish] Group: pulled member hwnd={} back into its slot "
                          L"(was {},{} {}x{}, restored to {},{} {}x{})",
                          reinterpret_cast<void*>(hwnd), current.left, current.top, current.right - current.left,
                          current.bottom - current.top, expected.left, expected.top,
                          expected.right - expected.left, expected.bottom - expected.top));
    return true;
}

void GroupManager::CaptureThumbnail(HWND hwnd) {
    RestoreIfMaximized(hwnd);

    // A member captured immediately after EnsureReparented's SetParent/
    // style change (no message pump in between, since the member
    // belongs to a different process's own thread) rendered only part
    // of its content, the rest solid black -- as if PrintWindow still
    // saw stale, pre-reparent layout. Confirmed via a compiled spike
    // against real File Explorer windows: both steps below measurably
    // shrink the blank region (from most of the window down to a thin
    // strip), though a residual strip can still remain for apps with
    // their own async/compositor-based chrome (confirmed even after a
    // full 3-second settle before ever touching the window) -- treated
    // as a known, not-fully-solved edge case rather than papered over.
    //   1. SendMessageW (not PostMessageW) blocks until hwnd's own
    //      thread has processed every message already queued ahead of
    //      this one, including the pending WM_NCCALCSIZE/WM_SIZE from
    //      the style/parent change.
    //   2. RedrawWindow(RDW_UPDATENOW) then forces a synchronous
    //      repaint at whatever layout the window now has. No RDW_ERASE
    //      -- confirmed via diagnostic logging that RDW_ERASE here
    //      visibly flashed a member's own header when it was captured
    //      while still on-screen mid tab-switch; RDW_UPDATENOW alone
    //      still forces the real fix, the synchronous WM_PAINT. hwnd is
    //      never actually hidden by this call (a non-active member
    //      stays WS_VISIBLE at all times now -- see ApplyTabLayout --
    //      just covered by whichever member is on top of the Z-order),
    //      so this redraw happens fully behind an opaque window and is
    //      never itself visible to the user.
    SendMessageW(hwnd, WM_NULL, 0, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);
    const BOOL captured = PrintWindow(hwnd, memDC, kPrintWindowRenderFullContent);
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (!captured) {
        DeleteObject(bitmap);
        return;
    }
    const auto it = memberThumbnails_.find(hwnd);
    if (it != memberThumbnails_.end()) {
        DeleteObject(it->second);
        it->second = bitmap;
    } else {
        memberThumbnails_[hwnd] = bitmap;
    }
}

HBITMAP GroupManager::CachedThumbnail(HWND hwnd) const {
    const auto it = memberThumbnails_.find(hwnd);
    return it == memberThumbnails_.end() ? nullptr : it->second;
}

bool GroupManager::RefreshThumbnail(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        return false;
    }
    // A Tab-mode member stays WS_VISIBLE at all times now (see
    // ApplyTabLayout's Z-order-only tab switching) -- IsWindowVisible
    // can no longer tell an active member apart from an inactive,
    // merely-covered one, since both are visible. Only the active
    // member (the one actually on top of the Z-order, doing double
    // duty as its own "thumbnail") is skipped here now.
    if (const GroupState* group = FindGroupContaining(hwnd);
        group != nullptr && group->ActiveWindow().has_value() && *group->ActiveWindow() == hwnd) {
        return false;
    }

    UINT32 oldFingerprint = 0;
    bool hadOld = false;
    if (const auto it = memberThumbnails_.find(hwnd); it != memberThumbnails_.end()) {
        BITMAP info{};
        GetObjectW(it->second, sizeof(info), &info);
        oldFingerprint = SampleFingerprint(it->second, info.bmWidth, info.bmHeight);
        hadOld = true;
    }

    CaptureThumbnail(hwnd);

    const auto it = memberThumbnails_.find(hwnd);
    if (it == memberThumbnails_.end()) {
        return false;  // capture failed (PrintWindow returned false) -- nothing to compare
    }
    if (!hadOld) {
        return true;  // first-ever capture for this member is always "changed"
    }
    BITMAP newInfo{};
    GetObjectW(it->second, sizeof(newInfo), &newInfo);
    const UINT32 newFingerprint = SampleFingerprint(it->second, newInfo.bmWidth, newInfo.bmHeight);
    return newFingerprint != oldFingerprint;
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
    RestoreIfMaximized(hwnd);
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
        //
        // No RDW_ERASE, though -- confirmed via diagnostic logging that
        // this exact call, on every single tab switch, correlates
        // precisely with a visible flash of Explorer's own header/
        // ribbon (not the rest of its content). RDW_ERASE forces
        // WM_ERASEBKGND across every one of Explorer's own child panes
        // before they repaint, and that erase-then-repaint step is what
        // was visibly flashing -- the same class of bug already fixed
        // twice this session for our own windows, just happening inside
        // a different process's child controls here, where there's no
        // WM_ERASEBKGND to override ourselves. RDW_UPDATENOW alone still
        // forces a synchronous WM_PAINT across every child (via
        // RDW_ALLCHILDREN), which is what actually resolves the stale-
        // surface content the erase was originally added alongside --
        // the erase itself was never the part fixing that.
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    RECT actual{};
    GetWindowRect(hwnd, &actual);
    return actual;
}

// Converts a chrome-client-relative rect into screen coordinates -- what
// an *attached* member's slot needs to be, since it's a top-level window
// (SetWindowPos for a window with no parent positions it on the screen,
// not relative to anything), unlike an embedded member's chrome-client-
// relative slot.
RECT ClientRectToScreen(HWND chrome, const RECT& clientRect) {
    POINT points[2] = {{clientRect.left, clientRect.top}, {clientRect.right, clientRect.bottom}};
    MapWindowPoints(chrome, HWND_DESKTOP, points, 2);
    return RECT{points[0].x, points[0].y, points[1].x, points[1].y};
}
}  // namespace

SIZE GroupManager::ApplyLayout(GroupState& group, HWND chromeWindow, const RECT& contentRectClientCoords,
                                int tileSplitterWidthPx) {
    // Collected rather than removed as they're found: group.Remove
    // mutates group.Members(), the very vector this range-for is
    // iterating, so removal has to wait until the loop is done.
    std::vector<HWND> unjoinable;
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr && IsWindow(member.window)) {
            if (!EnsureAttached(member.window, chromeWindow)) {
                unjoinable.push_back(member.window);
            }
        }
    }
    for (HWND hwnd : unjoinable) {
        // Couldn't be embedded *or* attached (see EnsureAttached's own
        // comment -- in practice this means the window stopped being
        // valid between the IsWindow check above and here) -- dropped
        // from the group rather than left in place to be retried every
        // single reflow forever, which is exactly what happened before
        // this existed (confirmed live: dozens of identical failures a
        // second against Calculator, back when attaching wasn't an
        // option and every UWP frame window hit this path).
        LogDebug(std::format(L"[Polish] Group: dropping unjoinable member hwnd={} from group id={}",
                              reinterpret_cast<void*>(hwnd), group.Id()));
        group.Remove(hwnd);
        memberRects_.erase(hwnd);  // never actually positioned, but harmless/defensive to clear either way
    }

    if (IsTiledMode(group.Mode())) {
        return ApplyTileLayout(group, chromeWindow, contentRectClientCoords, tileSplitterWidthPx);
    }
    return ApplyTabLayout(group, chromeWindow, contentRectClientCoords);
}

SIZE GroupManager::ApplyTabLayout(const GroupState& group, HWND chromeWindow, const RECT& contentRect) {
    const int requestedWidth = contentRect.right - contentRect.left;
    const int requestedHeight = contentRect.bottom - contentRect.top;
    int neededWidth = requestedWidth;
    int neededHeight = requestedHeight;
    const std::optional<HWND> active = group.ActiveWindow();

    // An *embedded* member stays WS_VISIBLE at all times -- switching
    // tabs only restacks Z-order (the active member to the top among its
    // siblings), never hides or shows anything. This replaced an
    // earlier hide-inactive/show-active design after a real, confirmed
    // report: switching tabs visibly flashed a member's own header
    // (Explorer specifically) even after the redraw flags layered on
    // top of the hide/show cycle were themselves ruled out one at a
    // time (RDW_ERASE removed from both PositionMember's and
    // CaptureThumbnail's redraw calls, thumbnail capture disabled
    // entirely) and the flash persisted through all of it. The
    // deciding test: switching between two *standalone* (non-grouped)
    // Explorer windows via title-bar clicks -- Z-order only, nothing
    // ever hidden -- never flashed, unlike this group's tab switch.
    // Hiding and later re-showing a window is a materially heavier
    // operation for DWM/the app than a plain Z-order restack, which is
    // presumably why. Since a non-active *embedded* member is only ever
    // *covered* now, never actually hidden, RefreshThumbnail/
    // CaptureThumbnail no longer key off IsWindowVisible to find capture
    // candidates -- see their own comments.
    //
    // None of that reasoning applies to an *attached* member, though: an
    // owned window is always above its owner in Z-order, so it cannot be
    // merely covered the way a sibling child can -- it has to actually
    // be hidden when its tab isn't active, same as this app's earlier
    // (abandoned, for embedded members) hide/show design. This is a real
    // behavioral difference a user can notice (Calculator's own window
    // blinking away rather than just being covered), but there's no
    // Z-order-only alternative available for a window that isn't a
    // sibling of anything.
    for (const GroupMember& member : group.Members()) {
        if (member.kind != GroupMemberKind::Window || member.window == nullptr || !IsWindow(member.window)) {
            continue;  // nested-group case -- v1 never populates this
        }
        const bool attached = IsAttached(member.window);
        const bool isActiveMember = active.has_value() && *active == member.window;
        const RECT slot = attached ? ClientRectToScreen(chromeWindow, contentRect) : contentRect;
        const bool visible = !attached || isActiveMember;
        RestoreIfMaximized(member.window);
        SetWindowPos(member.window, nullptr, slot.left, slot.top, slot.right - slot.left, slot.bottom - slot.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // The *requested* slot, not whatever GetWindowRect reports back
        // below -- that can be larger than requested (a window's own
        // declared minimum tracking size, see PositionMember's own
        // comment), and EnforceMemberRect already has its own carve-out
        // for that exact case rather than this needing to record the
        // clamped size as "correct." In the member's own natural space
        // -- see memberRects_'s own comment for why that differs by kind.
        memberRects_[member.window] = slot;
        RECT actual{};
        GetWindowRect(member.window, &actual);
        neededWidth = std::max(neededWidth, static_cast<int>(actual.right - actual.left));
        neededHeight = std::max(neededHeight, static_cast<int>(actual.bottom - actual.top));
    }
    // Z-order promotion only matters for an embedded active member --
    // an attached one is already always above its owner regardless, and
    // HWND_TOP would instead promote it above unrelated top-level
    // windows system-wide, which is not what "the active tab" means.
    if (active.has_value() && IsWindow(*active) && !IsAttached(*active)) {
        SetWindowPos(*active, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    return SIZE{neededWidth, neededHeight};
}

SIZE GroupManager::ApplyTileLayout(GroupState& group, HWND chromeWindow, const RECT& contentRect,
                                    int splitterWidthPx) {
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
        tileColumnBoundaries_.erase(group.Id());
        tileRowBoundaries_.erase(group.Id());
        return SIZE{requestedWidth, requestedHeight};
    }

    if (group.IsTileMaximized()) {
        const std::optional<HWND> active = group.ActiveWindow();
        if (active.has_value() && IsWindow(*active)) {
            // Every member gets the *same* full-content rect and stays
            // shown -- the active one is simply Z-ordered on top,
            // exactly ApplyTabLayout's own "one covers the rest, nothing
            // ever hidden" technique (see its own comment for why: an
            // earlier hide-inactive/show-active design here visibly
            // flashed a member's own header on every switch, confirmed
            // real). Using SW_HIDE for the non-maximized tiles here
            // would risk the identical class of bug for no real benefit
            // -- they're fully covered either way.
            int neededWidth = requestedWidth;
            int neededHeight = requestedHeight;
            for (HWND hwnd : windows) {
                const RECT slot = IsAttached(hwnd) ? ClientRectToScreen(chromeWindow, contentRect) : contentRect;
                const RECT actual = PositionMember(hwnd, slot, true);
                memberRects_[hwnd] = slot;  // requested, not actual -- see the Tab-layout loop's comment
                neededWidth = std::max(neededWidth, static_cast<int>(actual.right - actual.left));
                neededHeight = std::max(neededHeight, static_cast<int>(actual.bottom - actual.top));
            }
            if (!IsAttached(*active)) {
                // See the symmetric skip in ApplyTabLayout's own comment
                // -- HWND_TOP would promote an attached member above
                // unrelated top-level windows system-wide, not just
                // within the group.
                SetWindowPos(*active, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            // No grid while maximized -- same "nothing to render/hit-
            // test" contract as the 0-1-member case above.
            tileColumnBoundaries_.erase(group.Id());
            tileRowBoundaries_.erase(group.Id());
            return SIZE{neededWidth, neededHeight};
        }
        // No valid active window (e.g. it closed) -- fall through to
        // the normal grid below rather than leaving every member
        // full-size with nothing chosen to be on top.
    }

    const int count = static_cast<int>(windows.size());
    const GridShape shape = ComputeGridShape(group.Mode(), group.Alignment(), count);
    const int cols = shape.cols;
    const int rows = shape.rows;

    // User-adjustable column widths/row heights (each a fraction of the
    // content area's total width/height), falling back to an equal
    // split whenever they don't match the grid's current column/row
    // count -- a member added/removed reshapes the grid, so fractions
    // sized for the old shape don't carry over.
    std::vector<double> columnFractions = group.TileColumnFractions();
    if (columnFractions.size() != static_cast<size_t>(cols)) {
        columnFractions.assign(static_cast<size_t>(cols), 1.0 / cols);
        group.SetTileColumnFractions(columnFractions);
    }
    std::vector<double> rowFractions = group.TileRowFractions();
    if (rowFractions.size() != static_cast<size_t>(rows)) {
        rowFractions.assign(static_cast<size_t>(rows), 1.0 / rows);
        group.SetTileRowFractions(rowFractions);
    }

    // Splitters reserve real space between adjacent columns/rows --
    // members never overlap them (an earlier version drew the splitter
    // *over* the members' shared edge, which could visibly race with
    // their own repaints; confirmed real). The fractions above divide
    // up the *content-only* width/height, excluding all the reserved
    // gaps.
    const int colGapTotal = (cols - 1) * splitterWidthPx;
    const int rowGapTotal = (rows - 1) * splitterWidthPx;
    const int contentOnlyWidth = std::max(0, requestedWidth - colGapTotal);
    const int contentOnlyHeight = std::max(0, requestedHeight - rowGapTotal);

    // Cumulative column/row edges in *content-only* space (gaps
    // excluded) -- colEdges[c] is column c's left edge if gaps didn't
    // exist, colEdges[cols] is the content-only area's own right edge
    // (forced exactly, absorbing any rounding error from the
    // fraction*width truncation into the last column rather than
    // leaving a gap).
    std::vector<int> colEdges(static_cast<size_t>(cols) + 1, 0);
    for (int c = 0; c < cols; ++c) {
        colEdges[static_cast<size_t>(c) + 1] =
            colEdges[static_cast<size_t>(c)] +
            static_cast<int>(std::lround(columnFractions[static_cast<size_t>(c)] * contentOnlyWidth));
    }
    colEdges[static_cast<size_t>(cols)] = contentOnlyWidth;

    std::vector<int> rowEdges(static_cast<size_t>(rows) + 1, 0);
    for (int r = 0; r < rows; ++r) {
        rowEdges[static_cast<size_t>(r) + 1] =
            rowEdges[static_cast<size_t>(r)] +
            static_cast<int>(std::lround(rowFractions[static_cast<size_t>(r)] * contentOnlyHeight));
    }
    rowEdges[static_cast<size_t>(rows)] = contentOnlyHeight;

    // Per-column/row required size, like an HTML table's auto layout --
    // a single oversized member (its own minimum size bigger than its
    // slot) only grows its own column/row, not the whole grid uniformly.
    std::vector<int> colWidths(static_cast<size_t>(cols), 0);
    std::vector<int> rowHeights(static_cast<size_t>(rows), 0);

    for (int i = 0; i < count; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        // Shift right/down by however many whole gaps precede this
        // column/row, converting content-only edges into real,
        // gap-reserved screen coordinates.
        const int slotLeft = colEdges[static_cast<size_t>(col)] + col * splitterWidthPx;
        const int slotRight = colEdges[static_cast<size_t>(col) + 1] + col * splitterWidthPx;
        const int slotTop = rowEdges[static_cast<size_t>(row)] + row * splitterWidthPx;
        const int slotBottom = rowEdges[static_cast<size_t>(row) + 1] + row * splitterWidthPx;
        const RECT clientSlot{contentRect.left + slotLeft, contentRect.top + slotTop, contentRect.left + slotRight,
                               contentRect.top + slotBottom};
        HWND memberHwnd = windows[static_cast<size_t>(i)];
        const RECT slot = IsAttached(memberHwnd) ? ClientRectToScreen(chromeWindow, clientSlot) : clientSlot;
        const RECT actual = PositionMember(memberHwnd, slot, true);
        memberRects_[memberHwnd] = slot;  // requested, not actual -- see the Tab-layout loop's comment
        colWidths[static_cast<size_t>(col)] =
            std::max(colWidths[static_cast<size_t>(col)], static_cast<int>(actual.right - actual.left));
        rowHeights[static_cast<size_t>(row)] =
            std::max(rowHeights[static_cast<size_t>(row)], static_cast<int>(actual.bottom - actual.top));
    }

    // Cache boundaries for the chrome's splitter rendering/hit-testing,
    // in real (gap-reserved) coordinates -- the *center* of each
    // reserved gap, internal edges only (a grid of N columns has N-1
    // draggable boundaries between them; the outer two edges aren't
    // splitters).
    std::vector<int> columnBoundaries;
    for (int c = 1; c < cols; ++c) {
        columnBoundaries.push_back(colEdges[static_cast<size_t>(c)] + (c - 1) * splitterWidthPx +
                                    splitterWidthPx / 2);
    }
    tileColumnBoundaries_[group.Id()] = std::move(columnBoundaries);
    std::vector<int> rowBoundaries;
    for (int r = 1; r < rows; ++r) {
        rowBoundaries.push_back(rowEdges[static_cast<size_t>(r)] + (r - 1) * splitterWidthPx + splitterWidthPx / 2);
    }
    tileRowBoundaries_[group.Id()] = std::move(rowBoundaries);

    int neededWidth = colGapTotal;
    for (int w : colWidths) {
        neededWidth += w;
    }
    int neededHeight = rowGapTotal;
    for (int h : rowHeights) {
        neededHeight += h;
    }

    return SIZE{std::max(neededWidth, requestedWidth), std::max(neededHeight, requestedHeight)};
}

std::vector<int> GroupManager::TileColumnBoundaries(GroupId id) const {
    const auto it = tileColumnBoundaries_.find(id);
    return it == tileColumnBoundaries_.end() ? std::vector<int>{} : it->second;
}

std::vector<int> GroupManager::TileRowBoundaries(GroupId id) const {
    const auto it = tileRowBoundaries_.find(id);
    return it == tileRowBoundaries_.end() ? std::vector<int>{} : it->second;
}

void GroupManager::SetTileBoundary(GroupState& group, bool column, size_t index, int newPixelPosition,
                                    int totalSize, int splitterWidthPx) {
    if (totalSize <= 0) {
        return;
    }
    std::vector<double> fractions = column ? group.TileColumnFractions() : group.TileRowFractions();
    const size_t count = fractions.size();
    if (index + 1 >= count) {
        return;
    }

    // `newPixelPosition`/`totalSize` are in real (gap-reserved) space --
    // the same space TileColumnBoundaries/TileRowBoundaries report,
    // where this boundary is the *center* of its reserved splitter gap.
    // Convert into content-only space (gaps excluded), matching how
    // ApplyTileLayout's own fractions divide things up: subtract the
    // `index` whole gaps preceding this one, then step back from the
    // gap's center to its left edge.
    const int gapTotal = static_cast<int>(count - 1) * splitterWidthPx;
    const int contentOnlySize = std::max(1, totalSize - gapTotal);
    const int contentOnlyPosition =
        newPixelPosition - static_cast<int>(index) * splitterWidthPx - splitterWidthPx / 2;

    // The span this one boundary can move within: the edge just before
    // `index` and the edge just after `index + 1` -- everything outside
    // this pair stays exactly as it was.
    double prevEdgeFraction = 0.0;
    for (size_t i = 0; i < index; ++i) {
        prevEdgeFraction += fractions[i];
    }
    const double pairFraction = fractions[index] + fractions[index + 1];
    const double nextEdgeFraction = prevEdgeFraction + pairFraction;

    const double newFraction = static_cast<double>(contentOnlyPosition) / contentOnlySize;
    const double clamped = std::clamp(newFraction, prevEdgeFraction, nextEdgeFraction);

    fractions[index] = clamped - prevEdgeFraction;
    fractions[index + 1] = nextEdgeFraction - clamped;

    if (column) {
        group.SetTileColumnFractions(std::move(fractions));
    } else {
        group.SetTileRowFractions(std::move(fractions));
    }
}

}  // namespace polish

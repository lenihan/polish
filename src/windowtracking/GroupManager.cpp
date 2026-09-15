#include "windowtracking/GroupManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include "util/DarkMode.h"
#include "util/Logging.h"
#include "windowtracking/RectUtils.h"
#include "windowtracking/SwitcherVisibility.h"
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

// The minimized counterpart of RestoreIfMaximized above, and it exists
// for the same reason: SetWindowPos silently no-ops on size/position for
// an iconic window, so a member that is minimized when we try to place
// it ends up ignoring the slot entirely.
//
// This is a real, confirmed case for an *attached* member specifically:
// an earlier version of ApplyTabLayout hid one (SWP_HIDEWINDOW) when its
// tab wasn't active, and hiding a UWP ApplicationFrameWindow makes its
// frame host treat the app as minimized, leaving it WS_MINIMIZE
// afterwards -- SWP_SHOWWINDOW alone then made it visible again without
// un-minimizing it, so it came back as a minimized window that couldn't
// be seen at all (confirmed, human-reported, with Calculator). That's
// why an attached member is now covered rather than hidden (see
// ApplyTabLayout's own comment) -- but this still matters for the
// group-minimize case (SetAttachedMembersHidden), which still genuinely
// hides, and for EnsureAttached's own one-time join-time guard.
//
// SW_SHOWNOACTIVATE, not SW_RESTORE: both un-minimize, but this one
// doesn't activate the window -- a tab switch (or a group restore) must
// not steal focus out from under whatever the user is actually typing
// into.
void RestoreIfIconic(HWND hwnd) {
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
}

// The hWndInsertAfter value to pass to SetWindowPos so the positioned
// window ends up directly *above* `target` in Z-order.
//
// SetWindowPos's hWndInsertAfter places the positioned window directly
// BEHIND the handle passed in -- passing `target` itself therefore puts
// a window behind target, the opposite of "above it". That inversion
// shipped in two previous rounds (every attached-member Z-order call
// read `SetWindowPos(member, chromeWindow, ...)`, intending "above the
// chrome") and is exactly why Calculator rendered behind the group
// window, then stayed invisible, across both of those rounds -- neither
// one actually changed the Z-order in the intended direction. The
// correct handle is whatever window currently sits directly above
// `target` (GW_HWNDPREV -- "the previous window in Z-order" is the one
// nearer the viewer); inserting the new window after *that* one lands it
// directly above target instead. HWND_TOP is the equivalent when target
// is already frontmost (nothing precedes it).
//
// A live query, not something to cache -- Z-order shifts on every call,
// including this function's own callers' previous iterations (see
// ApplyTileLayout's grid loop, which calls this once per attached member
// and relies on each successive result reflecting the one before it).
HWND WindowDirectlyAbove(HWND target) {
    HWND above = GetWindow(target, GW_HWNDPREV);
    return (above != nullptr && above != target) ? above : HWND_TOP;
}

// How many device px to probe when measuring a member's own claimed
// resize band -- generous but bounded, so a member that (incorrectly)
// hit-tests a large swath of itself as its own resize border can't turn
// this into a long-running loop.
constexpr int kMaxResizeBandProbePx = 24;
// A hung member must not stall an entire layout pass -- SendMessageTimeout,
// not SendMessage, with a short timeout per probe point.
constexpr UINT kResizeBandProbeTimeoutMs = 100;

bool IsLeadingResizeHitCode(LRESULT hit, bool vertical) {
    if (vertical) {
        return hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT;
    }
    return hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT;
}

// The number of device px along `member`'s own leading edge (top in
// Horizontal alignment, left in Vertical -- whichever edge sits against
// this app's own tab strip/header) that the member's own frame
// hit-tests as its own resize border.
//
// Some custom-frame apps (confirmed live: File Explorer) hit-test and
// draw a resize cursor for a band of pixels along their own top edge
// entirely on their own, independent of WS_THICKFRAME -- which
// ReparentIntoGroup already strips -- and without ever deferring
// WM_SETCURSOR to this app's chrome via DefWindowProc (confirmed via
// dedicated WM_SETCURSOR/WM_NCHITTEST instrumentation: across 108
// WM_SETCURSOR messages the chrome received, the member's own class
// never once appeared as the target, only its *children*, which forward
// HTCLIENT/HTCAPTION but never the resize codes the member's own
// WM_NCHITTEST answered directly to its own move/size loop). So neither
// stripping styles nor overriding WM_SETCURSOR can ever reach this --
// the only remaining lever is keeping that band physically outside the
// member's own window rect (see this function's callers in
// ApplyTabLayout/ApplyTileLayout).
//
// Returns 0 for a member that claims no such band (the overwhelmingly
// common case -- most apps don't hit-test their own frame at all).
// Probed by walking WM_NCHITTEST along the member's own leading edge,
// starting exactly at its corner, until the answer stops being a
// leading-edge resize code.
// A plain opaque child of the group's chrome, laid over the strip of a
// member that the member's own frame hit-tests as a resize border (see
// MeasureLeadingResizeBand). Covering it is the entire point: the mouse
// lands on *this* window instead of the member, so the member's own
// process never gets the chance to answer WM_NCHITTEST/WM_SETCURSOR for
// those pixels and the resize cursor (and the drag it invites) simply
// cannot happen there.
//
// Chosen over the two alternatives deliberately, after both were tried
// and failed:
//   - Overriding WM_SETCURSOR on the chrome (the member never forwards
//     the message -- confirmed across 108 samples in one session's log).
//   - Shrinking the member's own rect away from the band, which does
//     nothing at all: the band belongs to the member's own window, so it
//     simply moves down with it and stays just as hoverable. (Shipped
//     once by mistake; this comment exists so it isn't tried a third
//     time.)
// This one depends on nothing but sibling Z-order between two windows
// this app owns the relationship between, which is why it works.
constexpr wchar_t kResizeBandOverlayClassName[] = L"PolishResizeBandOverlay";

LRESULT CALLBACK ResizeBandOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SETCURSOR:
            // The whole reason this window exists.
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_ERASEBKGND: {
            // Painted to match the chrome's own content-area fill
            // (PaintTabStrip's kContentColor, which is also the active
            // tab's color) so the strip reads as part of the group's
            // chrome rather than as a seam over the member.
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH brush = CreateSolidBrush(IsDarkModeEnabled() ? RGB(0x20, 0x20, 0x20) : RGB(0xFF, 0xFF, 0xFF));
            FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
            DeleteObject(brush);
            return 1;
        }
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

HWND CreateResizeBandOverlay(HWND chrome) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = ResizeBandOverlayProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kResizeBandOverlayClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&windowClass);
        classRegistered = true;
    }
    // WS_CLIPSIBLINGS so the member it overlaps can't paint over it.
    return CreateWindowExW(0, kResizeBandOverlayClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, chrome,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
}

// Descends to the deepest window of `member`'s own subtree at
// `screenPt` -- the same descent the OS itself performs when deciding
// which window handles the mouse there, and therefore which window's
// answer actually determines the cursor.
//
// Load-bearing, and the reason four earlier attempts at this missed:
// asking the *member* (File Explorer's top-level CabinetWClass) what a
// point is gets that window's own answer, but the window that really
// handles the mouse -- and sets the resize cursor -- is a child several
// levels down (TITLE_BAR_SCAFFOLDING_WINDOW_CLASS), whose own resize
// band is taller than what the top level reports. Confirmed live via
// cursor instrumentation: the top level said 4px, the child was showing
// size-NS well below that.
//
// Deliberately restricted to the member's own subtree rather than using
// WindowFromPoint: this app's band overlay is a *sibling* of the member
// and may already be covering the very pixels being measured, which
// would make the measurement depend on its own previous result.
HWND DeepestChildAt(HWND member, POINT screenPt) {
    HWND current = member;
    // Bounded -- a malformed/recursive child chain must not hang a
    // layout pass. Real chains here are 2-4 deep.
    for (int depth = 0; depth < 8; ++depth) {
        POINT clientPt = screenPt;
        ScreenToClient(current, &clientPt);
        const HWND child = ChildWindowFromPointEx(current, clientPt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (child == nullptr || child == current) {
            break;
        }
        current = child;
    }
    return current;
}

int MeasureLeadingResizeBand(HWND member, bool vertical) {
    RECT rect{};
    if (!GetWindowRect(member, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
        return 0;
    }
    for (int offset = 0; offset < kMaxResizeBandProbePx; ++offset) {
        const int x = vertical ? rect.left + offset : (rect.left + rect.right) / 2;
        const int y = vertical ? (rect.top + rect.bottom) / 2 : rect.top + offset;
        const HWND target = DeepestChildAt(member, POINT{x, y});
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(target, WM_NCHITTEST, 0, MAKELPARAM(x, y), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                 kResizeBandProbeTimeoutMs, &result) == 0) {
            // Timed out (or the member vanished mid-probe) -- stop here
            // rather than risk stalling a layout pass over it; whatever
            // band was confirmed up to this point is still applied.
            return offset;
        }
        if (!IsLeadingResizeHitCode(static_cast<LRESULT>(result), vertical)) {
            return offset;
        }
    }
    return kMaxResizeBandProbePx;
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
    if (taskbarList_ != nullptr) {
        taskbarList_->Release();
    }
}

ITaskbarList* GroupManager::TaskbarListInstance() {
    if (taskbarList_ != nullptr) {
        return taskbarList_;
    }
    ITaskbarList* taskbar = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList,
                                 reinterpret_cast<void**>(&taskbar))) ||
        taskbar == nullptr) {
        LogDebug(L"[Polish] Group: CoCreateInstance(CLSID_TaskbarList) failed -- attached members will keep their "
                 L"own taskbar button");
        return nullptr;
    }
    if (FAILED(taskbar->HrInit())) {
        LogDebug(L"[Polish] Group: ITaskbarList::HrInit failed -- attached members will keep their own taskbar "
                 L"button");
        taskbar->Release();
        return nullptr;
    }
    taskbarList_ = taskbar;
    return taskbarList_;
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
    SuppressTaskbarButton(hwnd);
    return true;
}

void GroupManager::SuppressTaskbarButton(HWND hwnd) {
    // An attached member is a plain top-level window, so nothing else
    // suppresses its own taskbar button the way an embedded member's
    // WS_CHILD status does automatically.
    //
    // Two mechanisms, because one alone was confirmed insufficient: a
    // single ITaskbarList::DeleteTab at join time returned success and
    // the button stayed on the taskbar anyway (human-reported, twice) --
    // the shell re-adds a button for a window it sees being shown, and
    // every layout pass shows this one (SWP_SHOWWINDOW). So:
    //   1. WS_EX_TOOLWINDOW, the actual documented "keep this off the
    //      taskbar" style. Cross-process style writes are refused for
    //      some fields on a UWP frame (GWLP_HWNDPARENT is, confirmed --
    //      see AttachToGroup), so this reads the style back and logs
    //      whether it stuck rather than assuming.
    //   2. DeleteTab regardless, re-applied on every layout pass (see
    //      the call in ApplyLayout) rather than once at join, so a button
    //      the shell re-adds gets taken away again promptly.
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) == 0) {
        SetLastError(ERROR_SUCCESS);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW);
        const DWORD setError = GetLastError();
        const bool stuck = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
        // Confirmed live that this is not enough on its own for a UWP
        // frame: the style reads back as set and the taskbar button
        // stays regardless. A hide/show cycle -- normally what makes the
        // shell re-evaluate taskbar membership after a style change --
        // was tried too, and also made no difference. A packaged app's
        // taskbar button is driven by its *package identity*, not purely
        // by its window, which is consistent with everything else this
        // class of window refuses (SetParent and GWLP_HWNDPARENT both
        // fail outright -- see AttachToGroup). Kept anyway because it
        // costs nothing and is correct for any non-UWP window that ever
        // takes the attached path; see docs/LIMITATIONS.md for the
        // UWP-specific gap.
        LogDebug(std::format(L"[Polish] Group: WS_EX_TOOLWINDOW on attached member hwnd={} -- {} (error {})",
                              reinterpret_cast<void*>(hwnd), stuck ? L"stuck" : L"did NOT stick", setError));
    }
    if (ITaskbarList* taskbar = TaskbarListInstance(); taskbar != nullptr) {
        const HRESULT hr = taskbar->DeleteTab(hwnd);
        if (FAILED(hr)) {
            LogDebug(std::format(L"[Polish] Group: ITaskbarList::DeleteTab failed for hwnd={}, hr=0x{:08x}",
                                  reinterpret_cast<void*>(hwnd), static_cast<unsigned long>(hr)));
        }
    }
    //   3. The one thing that actually works for a packaged app, since
    //      neither of the above does: the shell's own app-view interface
    //      (see SwitcherVisibility.h for what it is, why it's needed,
    //      and the two guards that keep an undocumented vtable from
    //      being dangerous). Hides the taskbar button *and* Windows'
    //      native Alt+Tab entry. Silently does nothing for a
    //      non-packaged window, which the two documented levers above
    //      already cover.
    SetWindowShownInSwitchers(hwnd, false);
}

void GroupManager::RestoreTaskbarButton(HWND hwnd) {
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TOOLWINDOW);
    }
    if (ITaskbarList* taskbar = TaskbarListInstance(); taskbar != nullptr) {
        taskbar->AddTab(hwnd);
    }
    // Load-bearing: a member that left the group has to become reachable
    // again, and for a packaged app this is the call that actually put
    // it out of reach.
    SetWindowShownInSwitchers(hwnd, true);
}

void GroupManager::ReleaseGroup(const GroupState& group) {
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr) {
            ReleaseMember(member.window);
        }
    }
    // Lazy sweep of overlay pools whose chrome is already gone. The
    // overlays are children of their chrome, so Windows destroys them
    // with it and there's nothing to clean up but the map entry itself --
    // and this can't clean up *this* group's entry, since the caller
    // hasn't destroyed the chrome yet at this point (and doesn't pass it
    // here anyway). Sweeping on each release keeps the map from growing
    // without bound across a session's worth of opened/closed groups.
    for (auto it = resizeBandOverlays_.begin(); it != resizeBandOverlays_.end();) {
        if (IsWindow(it->first)) {
            ++it;
        } else {
            it = resizeBandOverlays_.erase(it);
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
            // The symmetric undo of EnsureAttached's own suppression.
            RestoreTaskbarButton(hwnd);
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
    memberTopResizeBandPx_.erase(hwnd);
    memberLeftResizeBandPx_.erase(hwnd);
}

void GroupManager::PlaceResizeBandOverlay(HWND chrome, size_t index, const RECT& rect) {
    std::vector<HWND>& pool = resizeBandOverlays_[chrome];
    while (pool.size() <= index) {
        HWND overlay = CreateResizeBandOverlay(chrome);
        if (overlay == nullptr) {
            LogDebug(std::format(L"[Polish] Group: resize-band overlay creation FAILED for chrome={} (error {})",
                                  reinterpret_cast<void*>(chrome), GetLastError()));
            return;  // can't create it -- the band just stays uncovered
        }
        LogDebug(std::format(L"[Polish] Group: resize-band overlay {} created for chrome={}",
                              reinterpret_cast<void*>(overlay), reinterpret_cast<void*>(chrome)));
        pool.push_back(overlay);
    }
    // HWND_TOP among the chrome's children: every member has already been
    // positioned (and the active one Z-promoted) by the time a layout
    // pass calls this, so going to the top here is what actually puts
    // this above the member it's covering.
    SetWindowPos(pool[index], HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void GroupManager::HideUnusedResizeBandOverlays(HWND chrome, size_t usedCount) {
    const auto it = resizeBandOverlays_.find(chrome);
    if (it == resizeBandOverlays_.end()) {
        return;
    }
    for (size_t i = usedCount; i < it->second.size(); ++i) {
        ShowWindow(it->second[i], SW_HIDE);
    }
}

int GroupManager::ResizeBandPx(HWND member, bool vertical) {
    std::map<HWND, int>& cache = vertical ? memberLeftResizeBandPx_ : memberTopResizeBandPx_;
    if (const auto it = cache.find(member); it != cache.end()) {
        return it->second;
    }
    const int band = MeasureLeadingResizeBand(member, vertical);
    cache[member] = band;
    // Logged once per member/axis (this is the only place that measures,
    // and it caches) -- a zero here means the member claims no band of
    // its own and no overlay gets placed for it, which is exactly the
    // thing to check first if a resize cursor is still reachable. The
    // class name is the *deepest* child at the member's own leading
    // corner, i.e. the window whose answer actually governs the cursor
    // there (see DeepestChildAt) -- not necessarily the member itself.
    RECT rect{};
    wchar_t className[128] = L"";
    if (GetWindowRect(member, &rect)) {
        const POINT corner{vertical ? rect.left : (rect.left + rect.right) / 2,
                            vertical ? (rect.top + rect.bottom) / 2 : rect.top};
        GetClassNameW(DeepestChildAt(member, corner), className, static_cast<int>(std::size(className)));
    }
    LogDebug(std::format(L"[Polish] Group: member hwnd={} claims a {}px {} resize band of its own (governed by [{}])",
                          reinterpret_cast<void*>(member), band, vertical ? L"left" : L"top", className));
    return band;
}

bool GroupManager::HasAttachedMembers(const GroupState& group) const {
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr && IsAttached(member.window)) {
            return true;
        }
    }
    return false;
}

void GroupManager::RaiseAttachedMembers(const GroupState& group, HWND chromeWindow) {
    // Tab mode: only the *active* attached member belongs above the
    // chrome -- every other one is deliberately kept behind it (covered,
    // same as an embedded sibling -- see ApplyTabLayout's own comment),
    // and raising it here too would incorrectly surface a covered tab.
    // Tile/Stack: every member is visible in its own slot simultaneously,
    // so every attached one belongs above the chrome, same as
    // ApplyTileLayout's own grid loop.
    const std::optional<HWND> active = group.ActiveWindow();
    const bool tiled = IsTiledMode(group.Mode());
    for (const GroupMember& member : group.Members()) {
        if (member.kind != GroupMemberKind::Window || member.window == nullptr || !IsWindow(member.window) ||
            !IsAttached(member.window)) {
            continue;
        }
        if (tiled || (active.has_value() && *active == member.window)) {
            SetWindowPos(member.window, WindowDirectlyAbove(chromeWindow), 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

void GroupManager::SetAttachedMembersHidden(const GroupState& group, bool hide) {
    for (const GroupMember& member : group.Members()) {
        if (member.kind == GroupMemberKind::Window && member.window != nullptr && IsWindow(member.window) &&
            IsAttached(member.window)) {
            ShowWindow(member.window, hide ? SW_HIDE : SW_SHOWNOACTIVATE);
        }
    }
}

bool GroupManager::EnforceMemberRect(HWND hwnd) {
    const auto it = memberRects_.find(hwnd);
    if (it == memberRects_.end() || !IsWindow(hwnd)) {
        return false;
    }
    // A hidden member's geometry doesn't matter until it's shown again --
    // and showing it goes through ApplyLayout, not this. Load-bearing for
    // an *attached* member specifically: a deliberately-hidden inactive
    // tab (Tab mode, see ApplyTabLayout) reports its minimized-placeholder
    // rect from GetWindowRect while hidden, which will never match its
    // recorded slot, so without this bail every stray location-change
    // event for it fired a pointless SetWindowPos "correction" that
    // itself silently no-ops on an iconic window anyway.
    //
    // IsIconic as well as IsWindowVisible, and the iconic half is the
    // load-bearing one: a *minimized* window keeps WS_VISIBLE set, so
    // IsWindowVisible alone does not catch it. A minimized member reports
    // the (-32000,-32000) iconic placeholder rect, which can never match
    // its recorded slot, and the SetWindowPos "correction" below silently
    // no-ops on an iconic window -- so every location-change event fired
    // another correction, which fired another event, forever. Confirmed
    // live, and not subtly: 2,323,461 identical "pulled member back into
    // its slot (was -32000,-32000 ...)" lines in a single session's log.
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }
    // This runs off EVENT_SYSTEM_MOVESIZESTART/EVENT_OBJECT_LOCATIONCHANGE
    // -- i.e. right as (or just before) a drag would actually start -- so
    // it's the earliest, most frequent point to catch a member that
    // re-applied its own frame styles (see ReapplyChildFrameStyles's own
    // comment for why that matters); ApplyLayout's own call only runs
    // once per reflow, which can be much less often than every drag
    // attempt. Never for an attached member, same reasoning as
    // ApplyLayout's call.
    if (!IsAttached(hwnd)) {
        ReapplyChildFrameStyles(hwnd);
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
    //      never actually hidden by this call itself -- a non-active
    //      *embedded* member stays WS_VISIBLE at all times (see
    //      ApplyTabLayout), just covered by whichever member is on top
    //      of the Z-order, so this redraw happens fully behind an opaque
    //      window and is never itself visible to the user. That
    //      invariant doesn't hold for an *attached* member, though (it
    //      genuinely gets SWP_HIDEWINDOW'd when its tab isn't active --
    //      same reason) -- this call would be a no-visible-effect no-op
    //      against an already-hidden window in that case, not a risk of
    //      a visible flash.
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
// Restores hwnd first if it's still maximized or minimized (SetWindowPos
// silently no-ops on size/position otherwise -- confirmed M0 finding for
// maximized; the identical trap for minimized is what let a
// just-un-covered attached member come back as an un-showable icon, see
// RestoreIfIconic's own comment), then positions it into `rect`
// (client-area-relative coordinates for an embedded member, screen
// coordinates for an attached one -- see ClientRectToScreen) and shows
// it. Returns hwnd's *actual* resulting rect in screen coordinates
// (GetWindowRect always reports screen coordinates regardless of parent/
// child status) -- which can be larger than requested: SetWindowPos
// silently clamps to a window's own declared minimum tracking size
// rather than failing -- confirmed with both Notepad (small default
// chrome) and Outlook (a real user report) -- so a caller that only
// trusts the requested rect would leave that member visibly overflowing
// the group.
//
// Every member this is called for is shown at all times now, embedded or
// attached (see ApplyTabLayout's own comment on why an attached member
// is covered rather than hidden) -- there is currently no caller that
// wants this to hide hwnd instead.
//
// `insertAfter`, when non-null, is passed straight through as
// SetWindowPos's own hWndInsertAfter (hwnd ends up directly *behind* the
// window given -- see WindowDirectlyAbove's own comment if the goal is
// instead to end up in front of a particular window) instead of leaving
// Z-order untouched (SWP_NOZORDER) -- the manual stand-in for what an
// owned window's Z-order would otherwise guarantee for free, needed for
// an *attached* member (see GroupManager's own class comment). Left null
// (the default, and the only case for an embedded member, which is
// already a sibling child and whose Z-order Tab-mode active-promotion
// already owns) to keep today's SWP_NOZORDER behavior exactly.
RECT PositionMember(HWND hwnd, const RECT& rect, HWND insertAfter = nullptr) {
    RestoreIfMaximized(hwnd);
    RestoreIfIconic(hwnd);
    const UINT zOrderFlag = insertAfter != nullptr ? 0 : SWP_NOZORDER;
    SetWindowPos(hwnd, insertAfter, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 zOrderFlag | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // SWP_SHOWWINDOW makes it visible but doesn't guarantee its content
    // actually repaints -- confirmed real: switching tabs showed a blank
    // window until the user moved the mouse over it. A window that was
    // just hidden (or freshly reparented) can sit on a stale/
    // uncomposited DWM redirection surface until something forces it to
    // redraw; RDW_ALLCHILDREN covers apps like Explorer that are
    // themselves made of child panes.
    //
    // No RDW_ERASE, though -- confirmed via diagnostic logging that this
    // exact call, on every single tab switch, correlates precisely with
    // a visible flash of Explorer's own header/ribbon (not the rest of
    // its content). RDW_ERASE forces WM_ERASEBKGND across every one of
    // Explorer's own child panes before they repaint, and that
    // erase-then-repaint step is what was visibly flashing -- the same
    // class of bug already fixed twice this session for our own windows,
    // just happening inside a different process's child controls here,
    // where there's no WM_ERASEBKGND to override ourselves.
    // RDW_UPDATENOW alone still forces a synchronous WM_PAINT across
    // every child (via RDW_ALLCHILDREN), which is what actually resolves
    // the stale-surface content the erase was originally added alongside
    // -- the erase itself was never the part fixing that.
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
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
                continue;
            }
            // An embedded member that re-applies its own frame styles
            // (common around its own restore/DPI/theme handling) gets
            // its resize border -- and resize cursor -- back, which is
            // what let a member be dragged/resized inside the group
            // before EnforceMemberRect existed to catch the result.
            // Checked every layout pass, not just at join time. Never
            // for an attached member -- AttachToGroup deliberately
            // leaves a UWP frame's own styles alone.
            if (!IsAttached(member.window)) {
                ReapplyChildFrameStyles(member.window);
            } else {
                // Re-applied every pass, not just at join -- the shell
                // re-adds a taskbar button for a window it sees being
                // shown, and the layout below shows this one every time.
                // See SuppressTaskbarButton's own comment.
                SuppressTaskbarButton(member.window);
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
        memberTopResizeBandPx_.erase(hwnd);
        memberLeftResizeBandPx_.erase(hwnd);
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
    const bool vertical = group.Alignment() == GroupAlignment::Vertical;

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
    // An *attached* member follows the identical Z-order-only model: it
    // is not a sibling child (a top-level window positioned above or
    // below the chrome by hand -- see GroupManager's own class comment;
    // an owned window would have kept this for free, but
    // GWLP_HWNDPARENT is refused cross-process for a UWP frame the same
    // way SetParent is, confirmed live), but there is no *owner*
    // relationship it depends on either -- it's an ordinary top-level
    // window that can simply be placed behind the chrome to be covered,
    // exactly like an embedded sibling. An earlier version of this
    // instead hid a non-active attached member (SWP_HIDEWINDOW) on the
    // theory that an unparented window "can't" be covered -- that theory
    // was wrong (there's nothing stopping it), and hiding a UWP frame
    // specifically let its app get suspended and the frame window itself
    // destroyed, so the *next* tab switch found a dead hwnd and the
    // member silently vanished from the group (confirmed live: a fresh
    // Calculator hwnd showed up in the log's own Attach line, moments
    // after the group still referenced the old, now-closed one).
    // Covering instead of hiding removes that failure mode entirely, on
    // top of matching the embedded side's own no-flash reasoning above.
    for (const GroupMember& member : group.Members()) {
        if (member.kind != GroupMemberKind::Window || member.window == nullptr || !IsWindow(member.window)) {
            continue;  // nested-group case -- v1 never populates this
        }
        const bool attached = IsAttached(member.window);
        const bool isActiveMember = active.has_value() && *active == member.window;
        const RECT slot = attached ? ClientRectToScreen(chromeWindow, contentRect) : contentRect;
        RestoreIfMaximized(member.window);
        // Every member -- embedded or attached -- is shown at all times
        // now; see the loop's own comment above for why an attached
        // member is no longer ever hidden. RestoreIfIconic still matters
        // for one whose *group* was minimized (SetAttachedMembersHidden)
        // and is now coming back.
        RestoreIfIconic(member.window);
        // Attached, not the active tab: placed directly *behind* the
        // chrome so it's covered, same as an embedded sibling -- passing
        // chromeWindow itself as hWndInsertAfter does exactly that
        // (SetWindowPos's hWndInsertAfter places the positioned window
        // behind the handle given). Attached, the active tab: Z-order
        // left untouched here (SWP_NOZORDER) and handled once, below,
        // after every member has been placed -- computing/writing its
        // correct Z-order here too would just be redundant, since the
        // post-loop step below always runs and always gets the last
        // word. Embedded: Z-order untouched here regardless, exactly as
        // before (Tab-mode active-embedded promotion is the HWND_TOP
        // branch below, unchanged).
        const bool coverBehindChrome = attached && !isActiveMember;
        const HWND insertAfter = coverBehindChrome ? chromeWindow : nullptr;
        const UINT zOrderFlag = coverBehindChrome ? 0 : SWP_NOZORDER;
        SetWindowPos(member.window, insertAfter, slot.left, slot.top, slot.right - slot.left, slot.bottom - slot.top,
                     zOrderFlag | SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    // The active member's Z-order promotion, done once here rather than
    // inline in the loop above (see its own comment) -- HWND_TOP for an
    // embedded member (top of the whole sibling stack, same as always);
    // WindowDirectlyAbove(chromeWindow) for an attached one, since
    // HWND_TOP would instead promote it above unrelated top-level
    // windows system-wide, which is not what "the active tab" means.
    if (active.has_value() && IsWindow(*active)) {
        const HWND insertAfter = IsAttached(*active) ? WindowDirectlyAbove(chromeWindow) : HWND_TOP;
        SetWindowPos(*active, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Cover the active member's own resize band, last of all -- it has to
    // run after the Z-promotion just above, which would otherwise put the
    // member back on top of its own overlay. Only the active member needs
    // one in Tab mode: every other member is fully covered by it anyway,
    // so there's nothing of theirs left to hover. An attached member is
    // skipped -- it's a top-level window of a UWP frame, which doesn't
    // hit-test its own frame the way a custom-frame Win32 app does (see
    // MeasureLeadingResizeBand), and a child of this chrome couldn't
    // cover it regardless.
    size_t overlaysUsed = 0;
    if (active.has_value() && IsWindow(*active) && !IsAttached(*active)) {
        if (const int band = ResizeBandPx(*active, vertical); band > 0) {
            RECT bandRect = contentRect;
            if (vertical) {
                bandRect.right = bandRect.left + band;
            } else {
                bandRect.bottom = bandRect.top + band;
            }
            PlaceResizeBandOverlay(chromeWindow, overlaysUsed++, bandRect);
        }
    }
    HideUnusedResizeBandOverlays(chromeWindow, overlaysUsed);

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
    const bool vertical = group.Alignment() == GroupAlignment::Vertical;
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
            // real). An attached member follows the identical model, for
            // the identical reason -- see ApplyTabLayout's own comment on
            // why it's covered (placed behind the chrome) rather than
            // hidden.
            int neededWidth = requestedWidth;
            int neededHeight = requestedHeight;
            for (HWND hwnd : windows) {
                const bool attached = IsAttached(hwnd);
                const bool isActiveMember = hwnd == *active;
                const RECT slot = attached ? ClientRectToScreen(chromeWindow, contentRect) : contentRect;
                // Same split as ApplyTabLayout's own loop: attached,
                // non-active -> placed directly behind the chrome (covered);
                // attached, active -> Z-order left untouched here and
                // promoted once below; embedded -> untouched here either
                // way (Tab-mode-style active-embedded promotion is the
                // HWND_TOP branch below).
                const bool coverBehindChrome = attached && !isActiveMember;
                const RECT actual = PositionMember(hwnd, slot, coverBehindChrome ? chromeWindow : nullptr);
                memberRects_[hwnd] = slot;  // requested, not actual -- see the Tab-layout loop's comment
                neededWidth = std::max(neededWidth, static_cast<int>(actual.right - actual.left));
                neededHeight = std::max(neededHeight, static_cast<int>(actual.bottom - actual.top));
            }
            // The active member's Z-order promotion, done once here for
            // the same reason as ApplyTabLayout's own identical block.
            {
                const HWND insertAfter = IsAttached(*active) ? WindowDirectlyAbove(chromeWindow) : HWND_TOP;
                SetWindowPos(*active, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            // Only the maximized tile is visible, so only it needs its
            // band covered -- see ApplyTabLayout's own identical block.
            size_t maximizedOverlaysUsed = 0;
            if (!IsAttached(*active)) {
                if (const int band = ResizeBandPx(*active, vertical); band > 0) {
                    RECT bandRect = contentRect;
                    if (vertical) {
                        bandRect.right = bandRect.left + band;
                    } else {
                        bandRect.bottom = bandRect.top + band;
                    }
                    PlaceResizeBandOverlay(chromeWindow, maximizedOverlaysUsed++, bandRect);
                }
            }
            HideUnusedResizeBandOverlays(chromeWindow, maximizedOverlaysUsed);
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

    // Collected during the loop, placed after it -- every tile is visible
    // at once here, so each embedded one whose frame claims a band needs
    // its own overlay, and they all have to go on top of every member
    // rather than whichever member happens to be positioned after them.
    std::vector<RECT> bandRects;

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
        const bool attached = IsAttached(memberHwnd);
        const RECT slot = attached ? ClientRectToScreen(chromeWindow, clientSlot) : clientSlot;
        // Every member here occupies its own distinct tile, all visible
        // at once -- unlike Tab mode's "one covers the rest," there's no
        // member here that's supposed to be covered by the chrome, so an
        // attached one goes directly *above* it (WindowDirectlyAbove),
        // not behind. Recomputed fresh each iteration -- see that
        // function's own comment for why that matters when a group has
        // more than one attached member. Embedded: an ordinary sibling
        // child, Z-order untouched here as always.
        const RECT actual =
            PositionMember(memberHwnd, slot, attached ? WindowDirectlyAbove(chromeWindow) : nullptr);
        memberRects_[memberHwnd] = slot;  // requested, not actual -- see the Tab-layout loop's comment
        if (!attached) {
            if (const int band = ResizeBandPx(memberHwnd, vertical); band > 0) {
                RECT bandRect = clientSlot;
                if (vertical) {
                    bandRect.right = bandRect.left + band;
                } else {
                    bandRect.bottom = bandRect.top + band;
                }
                bandRects.push_back(bandRect);
            }
        }
        colWidths[static_cast<size_t>(col)] =
            std::max(colWidths[static_cast<size_t>(col)], static_cast<int>(actual.right - actual.left));
        rowHeights[static_cast<size_t>(row)] =
            std::max(rowHeights[static_cast<size_t>(row)], static_cast<int>(actual.bottom - actual.top));
    }
    for (size_t i = 0; i < bandRects.size(); ++i) {
        PlaceResizeBandOverlay(chromeWindow, i, bandRects[i]);
    }
    HideUnusedResizeBandOverlays(chromeWindow, bandRects.size());

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

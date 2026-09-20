#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "hook/ActiveWindowHalo.h"
#include "hook/AltTabDimOverlay.h"
#include "hook/AltTabHighlightBorder.h"
#include "hook/AltTabHook.h"
#include "hook/AltTabListWindow.h"
#include "hook/BullseyeOverlay.h"
#include "hook/GroupChromeWindow.h"
#include "hook/GroupHotkeyDialog.h"
#include "hook/GroupPickerWindow.h"
#include "hook/GroupTabThumbnail.h"
#include "settings/Settings.h"
#include "tabs/TabSwitching.h"
#include "tabs/UiaWorker.h"
#include "tray/TrayIcon.h"
#include "util/AnchorPoint.h"
#include "util/AppIdentity.h"
#include "util/Logging.h"
#include "util/SwitcherCycle.h"
#include "util/WindowIcon.h"
#include "windowtracking/ActivationHistory.h"
#include "windowtracking/GroupManager.h"
#include "windowtracking/RectUtils.h"
#include "windowtracking/WindowFilters.h"
#include "windowtracking/WindowZOrder.h"

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\Polish_SingleInstance_Mutex";
constexpr wchar_t kMessageWindowClassName[] = L"PolishMessageWindow";

// Debounce for treating a window as "settled" after it stops
// moving/resizing -- long enough to coalesce a drag's flood of
// LOCATIONCHANGE events into one, short enough to feel immediate.
constexpr UINT_PTR kSettleTimerId = 1;
constexpr UINT kSettleTimerDelayMs = 180;

// A freshly-captured tab thumbnail (GroupManager::CaptureThumbnail, run
// synchronously right after a member is hidden) can still be
// incomplete -- confirmed real via a compiled spike against File
// Explorer: some of its own content (e.g. enumerating drives for a
// "This PC" view) loads asynchronously, arriving after our capture
// already ran, no matter how long we force-repaint/flush messages
// beforehand. Re-capturing once more after a real delay, off the UI
// thread's own timer (not a blocking Sleep), catches content that
// finishes loading shortly after. Re-armed (not just started) on every
// reflow, so a burst of layout changes (e.g. interactively resizing a
// group) only triggers one sweep, after things go quiet.
constexpr UINT_PTR kThumbnailRefreshTimerId = 2;
constexpr UINT kThumbnailRefreshDelayMs = 1200;

// There's no universal Win32 signal for "this window has finished
// loading its own content" -- every app manages that internally
// without exposing it (GroupManager::RefreshThumbnail's own comment).
// So while a tab's thumbnail is actively being hovered, keep
// re-capturing it a few times a short interval apart, updating the
// shown popup each time content actually changed, and stop as soon as
// two captures in a row match (settled) or this many attempts run out
// -- bounds worst-case latency to kThumbnailStabilizeMaxAttempts *
// kThumbnailStabilizeIntervalMs (currently ~450ms) rather than polling
// indefinitely.
constexpr UINT_PTR kThumbnailStabilizeTimerId = 3;
constexpr UINT kThumbnailStabilizeIntervalMs = 150;
constexpr int kThumbnailStabilizeMaxAttempts = 3;

// Debounces a burst of EVENT_OBJECT_LOCATIONCHANGE events that each carry
// a *different* target size (e.g. dragging a resize border) into one
// real render, the same way kSettleTimerId debounces a settling drag into
// one commit -- without this, an event storm during a resize drag on a
// large window could ask ActiveWindowHalo to fully re-render faster than
// it can actually produce frames. Short enough to still feel live; see
// ActiveWindowHalo's class comment for the measured full-render cost this
// was tuned against.
constexpr UINT_PTR kHaloRenderTimerId = 4;
constexpr UINT kHaloRenderTimerDelayMs = 8;

// The bullseye copy/paste ring's frame tick -- unlike every timer above, a
// genuinely repeating one, killed by its own handler once the animation
// reports it has finished. ~60 Hz; the animation's own progress is
// wall-clock-based (see BullseyeOverlay), so a late tick just skips a frame.
constexpr UINT_PTR kBullseyeFrameTimerId = 5;

// How long to wait for UiaWorker to report the selection rect before
// giving up and aiming the bullseye with the cheap caret/cursor/window
// chain instead. The call measured 4-12ms across every app probed, so this
// is pure insurance against one that never answers -- short enough that a
// hung app delays the animation imperceptibly rather than losing it.
constexpr UINT_PTR kBullseyeAnchorTimerId = 7;
constexpr UINT kBullseyeAnchorTimeoutMs = 90;

// Holds the halo back until Windows has finished animating a window back
// up from the taskbar.
//
// Nothing here is waiting for the window to *move*: through that whole
// animation GetWindowRect already reports the window's final rect, because
// the growing window is a DWM visual effect and the window's own logical
// position is final from the moment it is restored. So the halo is not
// late, it is early -- drawn correctly around where the window is about to
// be, framing empty desktop until the animation catches up. There is no
// exposed per-frame rect to follow instead, so the only honest options are
// "wait" or "draw it early", and waiting looks right.
//
// A duration, because the animation is not observable. Measured live:
// EVENT_OBJECT_LOCATIONCHANGE fires exactly once for a restore, at the
// same moment as EVENT_SYSTEM_MINIMIZEEND and already carrying the final
// rect, with IsIconic already false -- there is no stream of intermediate
// rects to follow and no event when the animation finishes. Tuned by eye.
constexpr UINT_PTR kHaloRestoreTimerId = 6;
constexpr UINT kHaloRestoreDelayMs = 300;

// Windows seen going into the taskbar, so a later restore can be
// recognized as one. EVENT_SYSTEM_MINIMIZEEND and the restored window's
// foreground change race each other -- confirmed live, with the
// foreground change arriving first every time it was measured -- so
// whichever lands first has to start the wait, and the other must find
// it already started. Without this the foreground handler drew the halo
// before MINIMIZEEND could suppress it, and the delay did nothing at all.
std::set<HWND> g_minimizedWindows;

// Whether the minimize/restore animation is actually switched on -- it can
// be off system-wide (SystemPropertiesPerformance, or "Show animations in
// Windows" in Settings), and on a machine with it off there is nothing to
// wait for and the halo should appear at once.
// Begins (or re-arms) the hold-off that keeps the halo off screen until a
// window has finished animating back up from the taskbar. Idempotent, and
// safe to call from either of the two racing events.
void BeginHaloRestoreWait(HWND restored);

bool MinimizeAnimationEnabled() {
    ANIMATIONINFO info{};
    info.cbSize = sizeof(info);
    if (!SystemParametersInfoW(SPI_GETANIMATION, sizeof(info), &info, 0)) {
        return true;  // assume the default rather than flash the halo early
    }
    return info.iMinAnimate != 0;
}
constexpr UINT kBullseyeFrameIntervalMs = 16;

// Some apps write the clipboard more than once per user-visible copy;
// updates closer together than this count as one.
constexpr ULONGLONG kBullseyeCopyDebounceMs = 150;

HWND g_messageWindow = nullptr;
HWINEVENTHOOK g_foregroundHook = nullptr;
HWINEVENTHOOK g_locationChangeHook = nullptr;
HWINEVENTHOOK g_moveSizeStartHook = nullptr;
HWINEVENTHOOK g_moveSizeEndHook = nullptr;
HWINEVENTHOOK g_destroyHook = nullptr;
HWINEVENTHOOK g_nameChangeHook = nullptr;
HWINEVENTHOOK g_objectFocusHook = nullptr;
HWINEVENTHOOK g_minimizeStartHook = nullptr;
HWINEVENTHOOK g_minimizeEndHook = nullptr;
UINT g_taskbarCreatedMessage = 0;

std::unique_ptr<polish::TrayIcon> g_trayIcon;

// User-toggleable feature flags, loaded once at startup and updated (+
// persisted) whenever the tray menu checkboxes are toggled -- see
// PopulateTrayMenu/HandleTrayCommand.
polish::Settings g_settings;
std::unique_ptr<polish::AltTabHook> g_altTabHook;

// Alt+Tab switcher session state. The candidate list is refreshed on
// every cycle (see OnAltTabCycle), not just once at session start, so a
// window opening/closing/minimizing mid-session is reflected the next
// time Tab is pressed -- order-preserving once a session is already open
// (UpdateAltTabCandidatesPreservingOrder), so Tab walks down a list that
// holds still, not one that reshuffles survivors around on every press.
// On a multi-monitor setup, grouped by monitor (see
// GetMonitorsCurrentFirst) -- current monitor's windows first, then each
// other monitor's in turn -- per explicit user request: every window
// stays reachable, but Tab finishes the current monitor before
// continuing onto the next one, and each monitor gets its own list
// panel (g_altTabPanels) showing only its own windows.
// The overlay pool only ever grows (indices beyond the current
// candidate count just stay hidden, and are explicitly hidden again if the
// count shrinks -- see OnAltTabCycle), so repeated sessions don't pay
// window-creation cost more than once per "most windows ever open at once
// this run."
bool g_altTabSessionOpen = false;
std::vector<HWND> g_altTabCandidates;
size_t g_altTabHighlightIndex = 0;
// Minimized candidate windows -- a separate list from g_altTabCandidates
// (see PLAN.md's Alt+Tab-improvements M4), kept out of the normal
// Tab/Shift+Tab cycle entirely. Rebuilt fresh via EnumWindows on every
// cycle/navigate, same as the active list; unlike g_altTabCandidates it
// has no "preserve existing order mid-session" pass, since nothing this
// app does (no promote/demote pulse touches a minimized window) is
// expected to reshuffle it the way ApplyAltTabDimming's own Z-order pulse
// once did for the active list.
std::vector<HWND> g_altTabMinimized;
// True while arrow-navigation selection sits in the minimized section
// instead of the normal Tab-cycle (g_altTabHighlightIndex). Only Up/Down
// (OnAltTabNavigate) can set this; Tab/Shift+Tab (OnAltTabCycle) always
// clear it and re-focus the active section, per the plan's explicit
// "Tab/Shift+Tab always stay within the active section" requirement.
bool g_altTabSelectionInMinimized = false;
size_t g_altTabMinimizedHighlightIndex = 0;
std::vector<std::unique_ptr<polish::AltTabDimOverlay>> g_altTabOverlays;
// Every non-minimized candidate window -- what ApplyAltTabDimming dims.
// Separate from g_altTabCandidates (what Tab cycles and the panel lists)
// because an Alt+` session cycles tabs, not windows, and so has no
// candidate windows at all while still wanting everything dimmed behind
// its panel. g_altTabOverlays is index-parallel to this list, not to
// g_altTabCandidates.
std::vector<HWND> g_altTabDimTargets;

// --- Alt+` tab-switching session state (see TabSwitching.h) ---
//
// A tab session is a different shape from a window one: exactly one
// window is involved, so there is no per-monitor grouping, no minimized
// section and no Z-order promotion -- just a list of tab titles and a
// highlight. It therefore gets its own state rather than being squeezed
// into g_altTabCandidates, which holds HWNDs; a tab has no HWND at all.
std::unique_ptr<polish::UiaWorker> g_uiaWorker;
// The bullseye waiting on a selection rect, if one is in flight -- see
// PlayBullseye. Unset whenever no animation is pending an anchor.
std::optional<polish::BullseyePhase> g_pendingBullseyePhase;
uint64_t g_pendingBullseyeGeneration = 0;
// The window whose tabs this session is over, and the rule its tabs are
// read with. Frozen when the session opens: re-deriving either mid-
// session would let the session drift onto another window.
HWND g_tabSessionWindow = nullptr;
polish::TabRule g_tabSessionRule;
// As the worker returned them, in the app's own visual order.
std::vector<polish::TabTarget> g_tabs;
// Display order: indices into g_tabs, most-recently-used first. Kept
// apart from g_tabs rather than reordering it, because the worker
// activates a tab by its index in the list *it* built -- reordering that
// list here would silently switch to the wrong tab on commit.
std::vector<size_t> g_tabOrder;
// Index into g_tabOrder, not g_tabs.
size_t g_tabHighlightIndex = 0;
// Per-window most-recently-used tab order, as UIA runtime ids (see
// TabTarget::runtimeId). This is what makes pressing Alt+` twice toggle
// between the last two tabs, the way Alt+Tab does for windows.
//
// Built entirely from observation at enumeration time -- every session
// sees which tab is currently frontmost and promotes it -- plus this
// app's own commits. There is no UIA event subscription behind it: that
// would mean holding an accessibility listener on the foreground app for
// the whole session, which costs far more than it would buy. The gap
// that leaves is small and self-correcting: switching tabs by hand
// several times between two Alt+` presses is only observed as the last
// of those switches.
std::map<HWND, std::vector<std::vector<int>>> g_tabMru;
// The worker generation g_tabs came from, passed back on commit so a
// snapshot that has since been superseded can never activate the wrong
// tab. See UiaWorker::RequestActivate.
uint64_t g_tabGeneration = 0;
// True from the moment Alt+` is accepted until the session ends. That
// deliberately includes the gap between accepting the keystroke and the
// worker returning the tab list, during which nothing is on screen yet --
// reading tabs costs ~50ms and cannot be done on the hook thread, so a
// session necessarily exists before it can be painted. g_tabsPainted is
// what distinguishes the two.
bool g_tabSessionOpen = false;
bool g_tabsPainted = false;
// Set when Alt is released before the tab list arrives -- a quick Alt+`
// tap. The switch still happens once the list lands, so a tap behaves
// like Alt+Tab's single-tap swap instead of silently doing nothing.
bool g_tabCommitPending = false;
// What AltTabEligibility last resolved for the foreground window, stashed
// there (it has the window in hand) and promoted by OnAltTabCycle when
// the session actually opens. Same thread, and nothing in between can
// move the foreground window, so there is no race.
HWND g_pendingTabWindow = nullptr;
polish::TabRule g_pendingTabRule;
// The theme-aware glow around whichever window is currently focused (see
// ActiveWindowHalo's own class comment for why this is a separate class
// from AltTabHighlightBorder, not a generalization of it) -- a
// single persistent instance, unlike every overlay above (session-scoped)
// or below (one per group's chrome): this is Polish's first continuously
// rendering overlay. It's also what marks the highlighted candidate during
// an Alt+Tab session (only one window is ever highlighted at a time, unlike
// the dim overlays, so one instance serves both -- see ApplyAltTabDimming;
// g_haloTarget stays nullptr for that use). g_haloTarget mirrors whichever window the halo is
// currently shown around (nullptr while hidden), so callers that only
// have an HWND to compare against (e.g. OnWinEvent's LOCATIONCHANGE/
// MINIMIZESTART cases) don't need to ask the halo object itself.
std::unique_ptr<polish::ActiveWindowHalo> g_activeWindowHalo;
HWND g_haloTarget = nullptr;
// The foreground window UpdateActiveWindowHalo last evaluated, whether or
// not it currently qualifies for a halo (maximized, full-screen, ...) --
// unlike g_haloTarget, which is nullptr while the halo is hidden.
HWND g_haloWatched = nullptr;
// Set for the duration of g_haloTarget's Win11 minimize animation (between
// EVENT_SYSTEM_MINIMIZESTART and MINIMIZEEND) -- LOCATIONCHANGE fires
// continuously through that animation while IsIconic is still false, so
// without this the halo would fly down to the taskbar and pop along with
// the window instead of just disappearing.
bool g_haloMinimizeSuppressed = false;

// The copy/paste ring animation (see BullseyeOverlay) -- a single
// persistent, pre-created instance like the halo.
std::unique_ptr<polish::BullseyeOverlay> g_bullseye;
ULONGLONG g_lastBullseyeCopyTick = 0;

// One always-shown active-window list panel *per connected monitor* (see
// PLAN.md's Alt+Tab-improvements plan) -- each showing only its own
// monitor's subset of g_altTabCandidates, with a highlighted row only on
// whichever monitor's subset actually contains the globally-highlighted
// window. Enumerated and created once at startup (pre-warmed, same
// lifecycle as the Alt+Tab dim overlays) -- a monitor connected/
// disconnected while the app is already running isn't picked up until
// restart, an accepted v1 simplification.
struct AltTabMonitorPanel {
    HMONITOR monitor;
    std::unique_ptr<polish::AltTabListWindow> window;
};
std::vector<AltTabMonitorPanel> g_altTabPanels;
// The candidate list as of the panels' last full rebuild -- lets
// ApplyAltTabDimming tell "the set changed, rebuild rows" apart from
// "just the highlight moved, cheap path" (see ApplyAltTabDimming).
std::vector<HWND> g_altTabListWindowLastCandidates;
// Same purpose as g_altTabListWindowLastCandidates, for the minimized
// section -- either list changing forces a full panel rebuild.
std::vector<HWND> g_altTabListWindowLastMinimized;

// Most-recently-used window activation order, for the in-progress
// Alt+Tab replacement (see PLAN.md). Tracks *every* real window that
// becomes foreground, not just candidate windows -- filtering (candidate
// + non-minimized) happens where this list is consumed, not here.
polish::ActivationHistory g_activationHistory;

// Window groups (tab/tile) -- see PLAN.md "Window groups" and the
// group plan file. v1, M3: GroupManager owns the pure state; each
// group also gets a GroupChromeWindow (static tab-strip rendering only
// so far -- no click handling, no real member positioning yet). Keyed
// by GroupId, in parallel with GroupManager's own storage, rather than
// folded into GroupState itself -- chrome is a Win32 window resource,
// not part of the pure/testable state.
polish::GroupManager g_groupManager;
std::map<polish::GroupId, std::unique_ptr<polish::GroupChromeWindow>> g_groupChromeWindows;

// Which tile is "active" in a Tile/Stack group -- one ring per group,
// keyed the same way as g_groupChromeWindows (a single shared instance,
// used until this map replaced it, let one group's reflow silently
// Hide() a different group's ring -- see EnsureGroupActiveTileHighlight's
// own comment). Uses AltTabHighlightBorder (Alt+Tab itself now uses
// ActiveWindowHalo instead) -- nothing in
// AltTabHighlightBorder assumes its target is top-level, and a
// reparented member's GetWindowRect/DwmGetWindowAttribute both still
// return real screen coordinates. See
// OnObjectFocusChanged/UpdateGroupActiveTileHighlight.
std::map<polish::GroupId, std::unique_ptr<polish::AltTabHighlightBorder>> g_groupActiveTileHighlights;

// One shared hover-preview popup, reused across every group's tab strip
// (only one can ever be hovered at a time app-wide) -- created lazily
// on first hover, not at startup, since most sessions may never hover
// a tab at all.
std::unique_ptr<polish::GroupTabThumbnail> g_groupTabThumbnail;

// The member whose thumbnail is currently shown in g_groupTabThumbnail
// (nullptr when nothing's showing) and its tab's screen rect -- tracked
// so kThumbnailStabilizeTimerId knows what to keep re-checking/
// re-displaying while the user is still hovering it. See that timer's
// own comment for why a single capture isn't trusted.
HWND g_hoveredThumbnailMember = nullptr;
RECT g_hoveredThumbnailTabRect{};
bool g_hoveredThumbnailPreferRight = false;
int g_thumbnailStabilizeAttemptsLeft = 0;

// Forward-declared: defined near the rest of the group-management code
// (TriggerNewGroup etc.), further down; called from OnWinEvent's
// EVENT_OBJECT_NAMECHANGE case, above that point. No-op unless hwnd is
// a group member -- a member's title changing is common (browser tabs,
// unsaved-changes markers, etc.) and previously just sat stale in its
// group's tab label until something else (a tab click, a
// drag) happened to trigger a repaint.
void OnMemberTitleChanged(HWND hwnd);

// Forward-declared for the same reason as OnMemberTitleChanged above:
// called from OnWinEvent's new EVENT_OBJECT_FOCUS case, defined further
// down near ActivateGroupTile. hwnd is whatever just received keyboard
// focus, system-wide -- a no-op unless it turns out to be (or be nested
// inside) a Tile-mode group's member window.
void OnObjectFocusChanged(HWND hwnd);

// Forward-declared so ReflowGroupTo (defined further down) can call them
// after GroupManager::ApplyLayout drops a member that turned out to be
// unreparentable -- the chrome's own tab labels/icons otherwise stay
// stale (one tab too many) until some unrelated event happens to
// refresh them. Defined further down near the rest of the tab-label
// refresh call sites they already share.
std::vector<std::wstring> CollectMemberTitles(const polish::GroupState& group);
std::vector<HICON> CollectMemberIcons(const polish::GroupState& group);

// A member's geometry belongs entirely to GroupManager -- see
// GroupManager::EnforceMemberRect's own comment. Called from three of
// OnWinEvent's cases below (MOVESIZESTART/LOCATIONCHANGE/MOVESIZEEND)
// whenever hwnd is a group member, to snap back a drag/resize the
// member's own frame let through. A thin wrapper only so those call
// sites read as intent ("keep this in place") rather than reaching into
// g_groupManager directly three times.
void KeepGroupMemberInPlace(HWND hwnd) { g_groupManager.EnforceMemberRect(hwnd); }

// The window currently being live-tracked for settle events -- i.e. the
// foreground window, whenever it's a candidate window (see
// IsCandidateWindow). Only one window is tracked at a time; Phase 2
// multi-window tracking would observe all windows, not just the
// foreground one.
HWND g_trackedWindow = nullptr;

// True while g_trackedWindow is in an active drag (between
// EVENT_SYSTEM_MOVESIZESTART and MOVESIZEEND). While true, the settle
// timer is suppressed entirely -- MOVESIZEEND is the sole, authoritative
// commit point during a live drag. Without this gate, pausing briefly
// mid-drag (very normal) can let the debounce timer fire on an
// incidental in-between position, recording it as if it were a
// deliberate final one and corrupting the restore-position sync --
// confirmed happening in practice (a paused drag produced several
// spurious "settled" events, same size, different position).
bool g_inMoveSizeLoop = false;

// The most recent rect observed via LOCATIONCHANGE that hasn't been
// committed (synced to rcNormalPosition) yet (debounce still pending).
// A new, distinctly different rect arriving before the previous one's
// debounce fires forces an immediate commit of the previous one first,
// instead of silently losing it -- covers quick successive non-drag
// position changes (e.g. a keyboard snap immediately followed by
// another) that would otherwise have the first one's debounce timer
// reset away before it ever fires.
std::optional<RECT> g_pendingSettleRect;

void LogDpiAwareness() {
    const DPI_AWARENESS_CONTEXT context = GetThreadDpiAwarenessContext();
    const DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(context);

    const wchar_t* description = L"unknown";
    switch (awareness) {
        case DPI_AWARENESS_UNAWARE:
            description = L"unaware";
            break;
        case DPI_AWARENESS_SYSTEM_AWARE:
            description = L"system-aware";
            break;
        case DPI_AWARENESS_PER_MONITOR_AWARE:
            description = L"per-monitor-aware";
            break;
        default:
            break;
    }

    OutputDebugStringW(std::format(L"[Polish] DPI awareness: {}\n", description).c_str());

    if (awareness != DPI_AWARENESS_PER_MONITOR_AWARE) {
        OutputDebugStringW(
            L"[Polish] WARNING: expected Per-Monitor-V2 DPI awareness from app.manifest; "
            L"window positioning math will be wrong on non-96-DPI monitors.\n");
    }
}

// Whether hwnd is in a state where its GetWindowRect() reflects a
// meaningful "restore to" position -- excludes minimized (GetWindowRect
// returns a sentinel off-screen rect while iconic) and maximized
// (GetWindowRect returns the full-screen rect, which must never become
// the restore target -- that's the exact opposite of the bug
// SyncRestorePlacement exists to fix, below).
bool IsWindowInNormalState(HWND hwnd) { return !IsIconic(hwnd) && !IsZoomed(hwnd); }

// Deliberately raw GetWindowRect() -- no DWM extended-frame-bounds
// conversion. See the comment in RectUtils.h for why: that conversion
// was tried and caused a real position bug (correct size, offset
// position) when toggling between two Windows Snap states, because the
// invisible resize-border inset isn't a fixed per-window constant.
// Recording/restoring the exact same GetWindowRect() value needs no
// conversion at all.
std::optional<RECT> QueryCurrentWindowRect(HWND hwnd) {
    RECT windowRect;
    if (!GetWindowRect(hwnd, &windowRect)) {
        return std::nullopt;
    }
    return windowRect;
}

// Keeps Windows' own restore-position bookkeeping (WINDOWPLACEMENT's
// rcNormalPosition, the rect the native Maximize/Restore button pair
// reads from and writes to) in sync with wherever the window actually
// just settled. Windows does not do this automatically for Snap actions
// (drag-to-edge, Win+Arrow, Snap Layouts) the way it does for an
// ordinary move/resize -- restoring after maximizing a snapped window
// lands back at whatever the window's rect was *before* the snap, not
// at the snap itself. This is the actual feature: fix the native
// Maximize/Restore buttons to round-trip through the most recent snap,
// rather than adding a new gesture/UI to work around them.
//
// Setting rcNormalPosition to `rect` while leaving showCmd (and
// everything else in the struct) exactly as GetWindowPlacement just
// reported is invisible -- the window is already sitting exactly at
// `rect`, so this only updates where a *future* restore should go, not
// anything currently on screen.
void SyncRestorePlacement(HWND hwnd, const RECT& rect) {
    if (!g_settings.restoreSyncEnabled) {
        return;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) {
        return;
    }
    // Defensive: never called while zoomed/iconic in practice (callers
    // already gate on IsWindowInNormalState before committing a rect at
    // all), but showCmd is re-checked here too since this function's
    // correctness depends entirely on rcNormalPosition never being set
    // from a maximized/minimized rect.
    if (placement.showCmd == SW_SHOWMAXIMIZED || placement.showCmd == SW_SHOWMINIMIZED) {
        return;
    }
    placement.rcNormalPosition = rect;
    SetWindowPlacement(hwnd, &placement);
}

// Proactively syncs rcNormalPosition to hwnd's current rect right now,
// not just reactively on the *next* observed change -- otherwise a window
// that was already snapped before Polish ever tracked it (an "existing"
// window the user brings to foreground and, without moving it further,
// just clicks native Maximize then Restore on) would never get its stale,
// pre-Polish rcNormalPosition corrected, since no settle event would ever
// fire for it. This was confirmed as a real gap: a reactive-only sync
// works for windows the user actively re-snaps after Polish starts
// tracking them ("new" windows, in practice, since freshly-launched ones
// are the ones being actively positioned) but silently misses windows
// that are already sitting at their current position when first seen.
// Safe to call repeatedly/idempotently -- syncing rcNormalPosition to a
// window's own current rect while it's already in that exact state is a
// no-op in effect, just re-confirming what's already true.
void SyncRestorePlacementNow(HWND hwnd) {
    if (!IsWindowInNormalState(hwnd)) {
        return;
    }
    if (const auto rect = QueryCurrentWindowRect(hwnd); rect.has_value()) {
        SyncRestorePlacement(hwnd, *rect);
    }
}

// Commits `rect` as the settled rect for g_trackedWindow -- the single
// path all settle commits go through (debounce fire, MOVESIZEEND,
// eager-commit-on-distinct-change, and the flush-on-drag-start below),
// so logging and the restore-placement sync stay in one place.
void CommitRectForTrackedWindow(const RECT& rect) {
    if (g_trackedWindow == nullptr) {
        return;
    }
    SyncRestorePlacement(g_trackedWindow, rect);
    polish::LogDebug(std::format(L"[Polish] settled rect synced for hwnd={}: ({},{})-({},{})",
                                  reinterpret_cast<void*>(g_trackedWindow), rect.left, rect.top, rect.right,
                                  rect.bottom));
}

void CheckSettledRectAndRecord() {
    g_pendingSettleRect.reset();
    if (g_trackedWindow == nullptr || !IsWindow(g_trackedWindow) ||
        !IsWindowInNormalState(g_trackedWindow)) {
        return;
    }
    const auto rect = QueryCurrentWindowRect(g_trackedWindow);
    if (!rect.has_value()) {
        return;
    }
    CommitRectForTrackedWindow(*rect);
}

// True if hwnd is one of our own group chrome windows -- these pass
// IsCandidateWindow's plain Win32-style checks (visible, WS_CAPTION,
// no owner) just like any real application window, but their size and
// position are fully owned by GroupManager/ReflowGroupTo, not the
// user. Letting restore-sync track one anyway is a real, confirmed bug
// (not hypothetical): a brand-new group's chrome naturally becomes the
// foreground window right after creation, so it becomes g_trackedWindow
// -- and if anything then reports even a spurious location-changed
// event on it (a live spike confirmed a reparented member's own
// content keeps repainting for a moment right after being reparented,
// which is exactly the kind of activity that can trip this), restore-
// sync's own SetWindowPlacement call on the chrome fights with
// ReflowGroupTo's layout-owned resizing of that same window -- visibly
// flashing, nonstop, since each side's correction can retrigger the
// other. Confirmed by a single-Notepad group flashing continuously
// with zero further calls into GroupManager after the initial layout.
bool IsGroupChromeWindow(HWND hwnd) {
    for (const auto& [id, chrome] : g_groupChromeWindows) {
        if (chrome->Handle() == hwnd) {
            return true;
        }
    }
    return false;
}

// Whether hwnd belongs to this process -- used to keep our own windows
// (the hidden message window TrackPopupMenu briefly foregrounds, the tray
// menu itself, ...) from disturbing the halo, without excluding group
// chrome, which is ours too but a legitimate halo target. See
// UpdateActiveWindowHalo's own comment for why this check exists at all.
bool IsOwnProcessWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid != 0 && pid == GetCurrentProcessId();
}

// The OS's own "a full-screen Direct3D game or a presentation is running"
// signal -- shared by the halo (via CoversWholeMonitor) and bullseye, both
// of which shouldn't draw over such a thing.
bool IsPresentationOrFullScreenGame() {
    QUERY_USER_NOTIFICATION_STATE notificationState;
    return SUCCEEDED(SHQueryUserNotificationState(&notificationState)) &&
           (notificationState == QUNS_RUNNING_D3D_FULL_SCREEN || notificationState == QUNS_PRESENTATION_MODE);
}

// True if hwnd's own visible rect covers its entire monitor -- IsZoomed
// isn't enough, since most full-screen apps (games, video players,
// browser F11) are borderless WS_POPUP windows sized to the monitor
// rather than actually maximized. Deliberately rcMonitor, not rcWork
// (the halo shouldn't appear just because a window fills the space above
// the taskbar -- only when it fills the literal screen), with a small
// tolerance and <=/>= so a window even slightly larger than the monitor
// still counts.
bool CoversWholeMonitor(HWND hwnd) {
    if (IsPresentationOrFullScreenGame()) {
        // Beyond looking wrong, a halo window sitting at HWND_TOP over a
        // borderless-fullscreen game can knock DWM out of its
        // fullscreen-optimization path -- worth suppressing on this
        // signal alone, even for a window whose rect this check might
        // otherwise miss.
        return true;
    }
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }
    RECT rect;
    if (!polish::GetVisibleWindowRect(hwnd, rect)) {
        return false;
    }
    constexpr int kTolerance = 2;
    const RECT& screen = monitorInfo.rcMonitor;
    return rect.left <= screen.left + kTolerance && rect.top <= screen.top + kTolerance &&
           rect.right >= screen.right - kTolerance && rect.bottom >= screen.bottom - kTolerance;
}

// Shows/hides/repositions the active-window halo to match hwnd's current
// state -- called any time something might have changed which window
// should have it, modeled on UpdateGroupActiveTileHighlight below. Always
// safe to call speculatively.
void UpdateActiveWindowHalo(HWND hwnd) {
    if (!g_activeWindowHalo) {
        return;
    }
    // During an Alt+Tab session the same halo instance belongs to
    // ApplyAltTabDimming (it glows around the highlighted candidate, not
    // the foreground window) -- leave it exactly as-is rather than hiding
    // it out from under the session. The session start already hid it
    // (see OnAltTabCycle) and EndAltTabSession re-runs this afterward.
    if (g_altTabSessionOpen) {
        return;
    }
    if (!g_settings.haloEnabled || g_haloMinimizeSuppressed) {
        g_activeWindowHalo->Hide();
        g_haloTarget = nullptr;
        return;
    }
    // Own process and not group chrome -> leave the halo exactly as-is,
    // rather than hiding it. Without this, the tray menu blinks the halo
    // off every time it's opened: TrackPopupMenu needs
    // SetForegroundWindow on our own hidden message window first (see
    // CreateMessageWindow), and the menu window itself is ours too --
    // both would otherwise fail IsCandidateWindow below and hide the
    // halo, only for it to reappear once real focus returns. Group
    // chrome windows are ours *and* legitimate targets, hence the
    // exception -- without it, focusing a group would leave a stale halo
    // on whatever was focused before it.
    if (IsOwnProcessWindow(hwnd) && !IsGroupChromeWindow(hwnd)) {
        return;
    }
    // Re-validates hwnd is still a real, visible, non-cloaked candidate
    // every time this runs (not just once when it was first chosen as
    // g_haloTarget) -- a ShowWindow(SW_HIDE) on the halo's own current
    // target leaves no EVENT_OBJECT_DESTROY and no reliable foreground
    // change to react to, so re-checking here on every call this app
    // already makes for other reasons is what actually catches it.
    //
    // Remembered separately from g_haloTarget (which goes nullptr whenever
    // the halo is hidden): a maximized foreground window hides the halo,
    // and its restore fires only LOCATIONCHANGE -- no foreground change --
    // so the location handler needs to still know which window to
    // re-evaluate after it stops covering the monitor.
    g_haloWatched = hwnd;
    if (polish::IsCandidateWindow(hwnd) && IsWindowInNormalState(hwnd) && !CoversWholeMonitor(hwnd)) {
        g_activeWindowHalo->ShowAroundTarget(hwnd);
        g_haloTarget = hwnd;
    } else {
        g_activeWindowHalo->Hide();
        g_haloTarget = nullptr;
    }
}

// Starts a bullseye animation at the best guess of where the copy/paste
// just happened. The single funnel for both triggers (clipboard update,
// paste chord), modeled on UpdateActiveWindowHalo -- always safe to call
// speculatively; every suppression rule lives here.
// Plays the animation once an anchor has been settled on.
void StartBullseyeAt(polish::BullseyePhase phase, POINT point, const wchar_t* sourceName) {
    if (!g_bullseye) {
        return;
    }
    g_bullseye->Start(phase, point);
    if (!g_bullseye->IsActive()) {
        return;
    }
    // Re-arming an already-running timer id just restarts its interval.
    SetTimer(g_messageWindow, kBullseyeFrameTimerId, kBullseyeFrameIntervalMs, nullptr);
    polish::LogDebug(std::format(L"[Polish] Bullseye: {} anchor={} at=({},{})",
                                  phase == polish::BullseyePhase::Copy ? L"copy" : L"paste", sourceName,
                                  point.x, point.y));
}

// Everything the cheap, synchronous chain can work out, used both as the
// fallback when there is no selection to aim at and if the worker does not
// answer in time.
void StartBullseyeFromFallbackAnchor(polish::BullseyePhase phase) {
    const polish::Anchor anchor = polish::ResolveInteractionAnchor();
    if (anchor.source == polish::AnchorSource::None) {
        return;
    }
    const wchar_t* sourceName = anchor.source == polish::AnchorSource::Caret    ? L"caret"
                                 : anchor.source == polish::AnchorSource::Cursor ? L"cursor"
                                                                                  : L"window";
    StartBullseyeAt(phase, anchor.point, sourceName);
}

// Starts a bullseye animation at the best guess of where the copy/paste
// just happened. The single funnel for both triggers (clipboard update,
// paste chord), modeled on UpdateActiveWindowHalo -- always safe to call
// speculatively; every suppression rule lives here.
//
// The best answer by far is the middle of what is actually selected, but
// that only comes from UI Automation, which must not run on this thread
// (see UiaWorker). So the request goes to the worker and the animation
// starts when it answers -- measured at 4-12ms, well under a frame, and
// invisible in practice. kBullseyeAnchorTimeoutMs covers an app that never
// answers, falling back to the caret/cursor/window chain rather than
// dropping the animation.
void PlayBullseye(polish::BullseyePhase phase) {
    if (!g_bullseye || !g_settings.bullseyeEnabled) {
        return;
    }
    // The Alt+Tab session owns the screen (dim overlays, panels).
    if (g_altTabSessionOpen || IsPresentationOrFullScreenGame()) {
        return;
    }
    if (g_uiaWorker == nullptr) {
        StartBullseyeFromFallbackAnchor(phase);
        return;
    }
    g_pendingBullseyePhase = phase;
    g_pendingBullseyeGeneration = g_uiaWorker->RequestSelectionRect();
    SetTimer(g_messageWindow, kBullseyeAnchorTimerId, kBullseyeAnchorTimeoutMs, nullptr);
}

// The worker has answered (or been given up on): aim at the selection if
// it found one, otherwise fall back.
void ResolveBullseyeAnchor(bool timedOut) {
    KillTimer(g_messageWindow, kBullseyeAnchorTimerId);
    if (!g_pendingBullseyePhase.has_value()) {
        return;
    }
    const polish::BullseyePhase phase = *g_pendingBullseyePhase;
    g_pendingBullseyePhase.reset();

    if (!timedOut && g_uiaWorker != nullptr) {
        const polish::UiaWorker::SelectionSnapshot selection = g_uiaWorker->LatestSelection();
        if (selection.generation == g_pendingBullseyeGeneration) {
            if (selection.found) {
                const POINT centre{(selection.bounds.left + selection.bounds.right) / 2,
                                   (selection.bounds.top + selection.bounds.bottom) / 2};
                StartBullseyeAt(phase, centre, L"selection");
                return;
            }
            // No selection -- a paste, typically. The accessibility caret
            // is the next best thing and, unlike the Win32 one, exists in
            // apps that draw their own (Chromium, Electron).
            if (selection.caretFound) {
                const POINT centre{(selection.caret.left + selection.caret.right) / 2,
                                   (selection.caret.top + selection.caret.bottom) / 2};
                StartBullseyeAt(phase, centre, L"msaa-caret");
                return;
            }
            // Last resort before the cheap chain -- see uiaCaretFound.
            if (selection.uiaCaretFound) {
                const POINT centre{(selection.uiaCaret.left + selection.uiaCaret.right) / 2,
                                   (selection.uiaCaret.top + selection.uiaCaret.bottom) / 2};
                StartBullseyeAt(phase, centre, L"uia-caret");
                return;
            }
        }
    }
    StartBullseyeFromFallbackAnchor(phase);
}

// True if `descendant` has `ancestor` somewhere up its parent-process chain.
// WebView2-hosted apps (new Outlook, Teams, ...) write the clipboard from a
// msedgewebview2.exe child of the process that owns the visible window, so
// "same pid" alone would reject their real copies.
bool IsProcessDescendantOf(DWORD descendant, DWORD ancestor) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::map<DWORD, DWORD> parentOf;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        parentOf[entry.th32ProcessID] = entry.th32ParentProcessID;
    }
    CloseHandle(snapshot);

    DWORD current = descendant;
    // Bounded: guards against pid-reuse cycles in the parent links.
    for (int depth = 0; depth < 16; ++depth) {
        auto it = parentOf.find(current);
        if (it == parentOf.end()) {
            return false;
        }
        current = it->second;
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

// WM_CLIPBOARDUPDATE -- the clipboard just changed. Only plays for a copy
// the user actually made in the app they're looking at.
void OnClipboardUpdated() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastBullseyeCopyTick < kBullseyeCopyDebounceMs) {
        return;
    }
    // Cleared rather than filled (EmptyClipboard with nothing set after).
    if (CountClipboardFormats() == 0) {
        return;
    }
    // A background app writing the clipboard (a clipboard manager, a sync
    // tool) isn't something the user just did in front of them. Suppress
    // only on a *confident* mismatch -- a null owner (delayed rendering,
    // or an owner-less writer) still plays, since a false negative on a
    // real copy is worse than a stray animation. "Match" means the same
    // process or a parent/child of it (see IsProcessDescendantOf).
    HWND owner = GetClipboardOwner();
    DWORD ownerPid = 0;
    DWORD foregroundPid = 0;
    if (owner != nullptr) {
        GetWindowThreadProcessId(owner, &ownerPid);
        if (HWND foreground = GetForegroundWindow()) {
            GetWindowThreadProcessId(foreground, &foregroundPid);
        }
    }
    if (ownerPid != 0 && foregroundPid != 0 && ownerPid != foregroundPid && ownerPid != GetCurrentProcessId() &&
        !IsProcessDescendantOf(ownerPid, foregroundPid) && !IsProcessDescendantOf(foregroundPid, ownerPid)) {
        return;
    }
    g_lastBullseyeCopyTick = now;
    PlayBullseye(polish::BullseyePhase::Copy);
}

void BeginHaloRestoreWait(HWND restored) {
    g_minimizedWindows.erase(restored);
    if (!MinimizeAnimationEnabled()) {
        return;  // nothing to wait for on a machine with animation off
    }
    if (g_haloMinimizeSuppressed) {
        return;  // the other of the two racing events already started it
    }
    // Hiding, not just flagging: the racing foreground change may already
    // have put the halo on screen, and a flag alone leaves it there --
    // which is exactly how the delay came to do nothing.
    if (g_activeWindowHalo) {
        g_activeWindowHalo->Hide();
        g_haloTarget = nullptr;
    }
    g_haloMinimizeSuppressed = true;
    KillTimer(g_messageWindow, kHaloRenderTimerId);
    SetTimer(g_messageWindow, kHaloRestoreTimerId, kHaloRestoreDelayMs, nullptr);
}

void OnForegroundChanged(HWND newForeground) {
    // A window coming to the front that went into the taskbar earlier is a
    // restore, and is usually how this app hears about one first -- before
    // EVENT_SYSTEM_MINIMIZEEND. Start the hold-off here so the halo is
    // never drawn over the still-animating window.
    if (g_minimizedWindows.count(newForeground) != 0) {
        BeginHaloRestoreWait(newForeground);
    }
    // Before the early return below -- the halo needs to react to every
    // real foreground change, including ones that don't touch
    // g_trackedWindow at all (e.g. a group chrome window, which
    // OnForegroundChanged otherwise ignores entirely -- see `candidate`
    // below).
    UpdateActiveWindowHalo(newForeground);

    if (newForeground == g_trackedWindow) {
        return;
    }
    KillTimer(g_messageWindow, kSettleTimerId);
    g_inMoveSizeLoop = false;
    g_pendingSettleRect.reset();
    g_trackedWindow = nullptr;

    // Still counted for MRU/Alt+Tab purposes below (a group chrome
    // window belongs in Alt+Tab like any real window) -- only excluded
    // from becoming g_trackedWindow/restore-sync's target, per
    // IsGroupChromeWindow's own comment above.
    const bool candidate = polish::IsCandidateWindow(newForeground) && !IsGroupChromeWindow(newForeground);
    wchar_t title[256] = L"";
    GetWindowTextW(newForeground, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
    polish::LogDebug(std::format(L"[Polish] foreground changed: hwnd={} title=\"{}\" candidate={}",
                                  reinterpret_cast<void*>(newForeground), title, candidate));

    // Tracks every real window that becomes foreground, not just
    // candidate windows -- Alt+Tab candidate filtering (candidate +
    // non-minimized) happens where this list is consumed, not here.
    g_activationHistory.MoveToFront(newForeground);
    std::wstring mruOrder;
    for (HWND hwnd : g_activationHistory.OrderedWindows()) {
        if (!mruOrder.empty()) {
            mruOrder += L", ";
        }
        mruOrder += std::format(L"{}", reinterpret_cast<void*>(hwnd));
    }
    polish::LogDebug(std::format(L"[Polish] MRU order ({}): {}", g_activationHistory.OrderedWindows().size(),
                                  mruOrder));

    if (!candidate) {
        return;
    }

    g_trackedWindow = newForeground;
    SyncRestorePlacementNow(newForeground);
}

void CALLBACK OnWinEvent(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                          DWORD /*eventThread*/, DWORD /*eventTime*/) {
    // The halo's own UpdateLayeredWindow/SetWindowPos calls generate a
    // LOCATIONCHANGE per rendered frame -- without this guard it falls
    // through to KeepGroupMemberInPlace's map lookup (harmless, just
    // wasted work) below every single time the halo moves or redraws.
    if (g_activeWindowHalo && hwnd == g_activeWindowHalo->Handle()) {
        return;
    }
    // Same for bullseye's topmost overlay, which updates every animation frame.
    if (g_bullseye && hwnd == g_bullseye->Handle()) {
        return;
    }
    switch (event) {
        case EVENT_SYSTEM_FOREGROUND:
            OnForegroundChanged(hwnd);
            break;

        case EVENT_SYSTEM_MOVESIZESTART:
            if (hwnd == g_trackedWindow) {
                KillTimer(g_messageWindow, kSettleTimerId);
                if (g_pendingSettleRect.has_value()) {
                    // A debounce-only change (e.g. a keyboard snap) was
                    // still waiting to commit when a live drag started --
                    // the drag starting is itself proof that state was
                    // final, so commit it now rather than losing it.
                    CommitRectForTrackedWindow(*g_pendingSettleRect);
                    g_pendingSettleRect.reset();
                }
                g_inMoveSizeLoop = true;
            }
            if (g_groupManager.FindGroupContaining(hwnd) != nullptr) {
                // A group member's own frame (Explorer's, a browser's,
                // ...) is about to enter its native move/size modal
                // loop -- confirmed real: dragging a member's resize
                // border resized it in place, revealing the other
                // members Z-ordered behind it (Tab mode never hides
                // them, see ApplyTabLayout's own comment). WM_CANCELMODE
                // is the documented way to abort that loop from outside
                // it; posted, not sent, so a hung/slow member can't
                // block this hook callback. EnforceMemberRect is the
                // belt to this loop's suspenders -- it also covers the
                // (likely, unverified) case where cancelling didn't
                // actually stop something that already moved a pixel or
                // two before this event was delivered.
                PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
                KeepGroupMemberInPlace(hwnd);
            }
            break;

        case EVENT_OBJECT_LOCATIONCHANGE:
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
                // Cheap for the overwhelming majority of these events (a
                // map lookup that immediately misses) -- see
                // KeepGroupMemberInPlace's own comment. Note the chrome
                // itself moving fires this for every child too, which is
                // harmless: memberRects_ is chrome-client-relative, and
                // a child's client-relative position doesn't change when
                // its parent moves.
                KeepGroupMemberInPlace(hwnd);
            }
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd == g_trackedWindow &&
                !g_inMoveSizeLoop) {
                // Only debounce outside an active drag -- see
                // g_inMoveSizeLoop's comment for why.
                if (!IsWindowInNormalState(hwnd)) {
                    // Maximizing/minimizing also fires LOCATIONCHANGE --
                    // don't treat that transition as a position to
                    // remember or sync (see IsWindowInNormalState).
                    g_pendingSettleRect.reset();
                    KillTimer(g_messageWindow, kSettleTimerId);
                } else if (const auto currentRect = QueryCurrentWindowRect(hwnd); currentRect.has_value()) {
                    if (g_pendingSettleRect.has_value() &&
                        !polish::RectsApproximatelyEqual(*g_pendingSettleRect, *currentRect)) {
                        // The window jumped to a distinctly different rect
                        // before the previous one's debounce fired --
                        // likely two separate, quick, discrete actions
                        // (e.g. a keyboard snap immediately followed by
                        // another). Commit the previous one now so it
                        // isn't silently lost.
                        CommitRectForTrackedWindow(*g_pendingSettleRect);
                    }
                    g_pendingSettleRect = currentRect;
                    SetTimer(g_messageWindow, kSettleTimerId, kSettleTimerDelayMs, nullptr);
                }
            }
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd == g_haloWatched &&
                !g_haloMinimizeSuppressed && !g_altTabSessionOpen) {
                // Same size as the halo's own last render -> the cheap
                // move-only path, safe to call inline on every event, no
                // matter how often they arrive. A different size (an
                // actual resize-border drag) instead (re)arms a short
                // debounce timer, so an event storm during that drag
                // can't ask for full re-renders faster than
                // ActiveWindowHalo can actually produce them -- same
                // reasoning as kSettleTimerId's own debounce, just for
                // rendering cost instead of a settle commit.
                RECT currentTargetRect;
                if (polish::GetVisibleWindowRect(hwnd, currentTargetRect)) {
                    const SIZE currentSize{currentTargetRect.right - currentTargetRect.left,
                                            currentTargetRect.bottom - currentTargetRect.top};
                    const SIZE cachedSize = g_activeWindowHalo->CachedTargetSize();
                    if (currentSize.cx == cachedSize.cx && currentSize.cy == cachedSize.cy) {
                        KillTimer(g_messageWindow, kHaloRenderTimerId);
                        UpdateActiveWindowHalo(hwnd);
                    } else if (g_inMoveSizeLoop) {
                        // A resize-border drag, which can fire these faster
                        // than a full re-render can keep up with -- debounce.
                        SetTimer(g_messageWindow, kHaloRenderTimerId, kHaloRenderTimerDelayMs, nullptr);
                    } else {
                        // A size change outside any drag: the shell
                        // animating a window back up from the taskbar,
                        // typically. Debouncing here does not merely delay
                        // the halo, it drops it for the whole animation --
                        // every frame re-arms the timer (SetTimer restarts
                        // it), so it cannot fire until the animation ends,
                        // and the halo snaps into place at the end instead
                        // of growing with the window. Worse, the one render
                        // that did happen was from before the shell finished
                        // raising the window, so the halo was left stranded
                        // behind it in the Z-order -- which is why a window
                        // restored from the taskbar showed no halo at all
                        // the first time and a correct one the second.
                        // Rendering inline also re-asserts Z placement (see
                        // PlaceHaloBehindTarget) on every animation frame.
                        KillTimer(g_messageWindow, kHaloRenderTimerId);
                        UpdateActiveWindowHalo(hwnd);
                    }
                }
            }
            break;

        case EVENT_SYSTEM_MOVESIZEEND:
            if (hwnd == g_trackedWindow) {
                g_inMoveSizeLoop = false;
                KillTimer(g_messageWindow, kSettleTimerId);
                CheckSettledRectAndRecord();
            }
            // The final word once a drag loop survives the MOVESIZESTART
            // cancel above (e.g. an app that runs its own drag loop
            // rather than the system one, so WM_CANCELMODE had nothing
            // to abort) -- LOCATIONCHANGE's own snap-back already fires
            // live during the drag, but this guarantees the end state
            // regardless.
            KeepGroupMemberInPlace(hwnd);
            break;

        case EVENT_OBJECT_DESTROY:
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
                g_activationHistory.Remove(hwnd);
                // A window closed while minimized would otherwise sit in
                // here forever, and HWNDs are recycled -- a later window
                // reusing the handle would be mistaken for a restore.
                g_minimizedWindows.erase(hwnd);
                if (hwnd == g_trackedWindow) {
                    g_trackedWindow = nullptr;
                    g_inMoveSizeLoop = false;
                    g_pendingSettleRect.reset();
                    KillTimer(g_messageWindow, kSettleTimerId);
                }
                if (hwnd == g_haloWatched) {
                    g_haloWatched = nullptr;
                }
                if (hwnd == g_haloTarget && g_activeWindowHalo) {
                    g_activeWindowHalo->Hide();
                    g_haloTarget = nullptr;
                    KillTimer(g_messageWindow, kHaloRenderTimerId);
                }
            }
            break;

        case EVENT_SYSTEM_MINIMIZESTART:
            g_minimizedWindows.insert(hwnd);
            if (hwnd == g_haloTarget && g_activeWindowHalo) {
                // LOCATIONCHANGE fires continuously through Win11's
                // minimize animation while IsIconic is still false, so
                // without this suppression the halo would fly down to the
                // taskbar and pop along with the window instead of just
                // disappearing.
                g_activeWindowHalo->Hide();
                g_haloMinimizeSuppressed = true;
                KillTimer(g_messageWindow, kHaloRenderTimerId);
                // Minimized again before a previous restore finished
                // animating -- drop the pending reveal with it.
                KillTimer(g_messageWindow, kHaloRestoreTimerId);
            }
            break;

        case EVENT_SYSTEM_MINIMIZEEND:
            // Stay suppressed until the restore animation has played out;
            // see kHaloRestoreTimerId for why this is a wait rather than a
            // follow. The suppression flag is already respected by both
            // UpdateActiveWindowHalo and the LOCATIONCHANGE handler, so the
            // burst of events the animation produces stays ignored until
            // the timer lifts it.
            BeginHaloRestoreWait(hwnd);
            if (!g_haloMinimizeSuppressed) {
                UpdateActiveWindowHalo(GetForegroundWindow());
            }
            break;

        case EVENT_OBJECT_NAMECHANGE:
            // idObject/idChild filtered to the window's own title bar
            // text specifically (not some arbitrary child control's
            // accessible name, which fires far more often) -- see
            // OnMemberTitleChanged's forward declaration for why this
            // is only meaningful to a group member.
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
                OnMemberTitleChanged(hwnd);
            }
            break;

        case EVENT_OBJECT_FOCUS:
            // Deliberately *not* filtered by idObject/idChild the way
            // the cases above are -- a real click almost always lands
            // on some nested control several levels inside a member's
            // own window tree (an edit control, a list view, ...), not
            // the member's own top-level client area, and this is
            // exactly the signal meant to catch focus at any depth (see
            // OnObjectFocusChanged's forward declaration). Its own
            // GetParent walk + small map lookup is cheap enough to run
            // unfiltered; the early exit for "not inside any group
            // chrome" handles the overwhelming majority of focus
            // changes system-wide.
            OnObjectFocusChanged(hwnd);
            break;

        default:
            break;
    }
}

// The monitor a session's list panel/dim overlays initially orient
// around -- the one containing the current foreground window, matching
// this feature's existing monitor-placement decision. Computed fresh
// every rebuild (not cached), but stable for the life of a session in
// practice: the foreground window itself never changes mid-session (only
// a commit changes it), so this returns the same monitor on every cycle
// of the same Alt-hold.
HMONITOR GetForegroundMonitor() { return MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST); }

BOOL CALLBACK EnumMonitorsProc(HMONITOR monitor, HDC /*hdc*/, LPRECT /*rect*/, LPARAM lParam) {
    reinterpret_cast<std::vector<HMONITOR>*>(lParam)->push_back(monitor);
    return TRUE;
}

// Every connected monitor, current-monitor-first then the rest in their
// original (stable) EnumDisplayMonitors order -- the order
// RebuildAltTabCandidates/UpdateAltTabCandidatesPreservingOrder group
// candidates by, so that a flat Tab/Shift+Tab walk over the resulting
// list finishes the current monitor's windows before continuing onto the
// next one. std::stable_partition (not a full sort) is exactly "move the
// current monitor to the front, leave everyone else's relative order
// alone" -- there's no other meaningful ordering to impose between
// monitors this app has no other opinion about.
std::vector<HMONITOR> GetMonitorsCurrentFirst() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&monitors));
    const HMONITOR current = GetForegroundMonitor();
    std::stable_partition(monitors.begin(), monitors.end(), [current](HMONITOR m) { return m == current; });
    return monitors;
}

BOOL CALLBACK EnumCandidateWindowsProc(HWND hwnd, LPARAM lParam) {
    // A group member is reachable through its group, not on its own --
    // the group's chrome is the single Alt+Tab entry standing in for all
    // of them, which is the whole point of grouping. A member excludes
    // itself for free (it's WS_CHILD, so EnumWindows never offers it
    // here at all).
    if (polish::IsCandidateWindow(hwnd) && !IsIconic(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

// Builds the candidate list from *every* currently open, real,
// non-minimized window (via EnumWindows), not just ones already present
// in g_activationHistory -- a window Polish has never seen become
// foreground (e.g. it's been sitting untouched since before this run
// started) would otherwise silently never appear as an Alt+Tab candidate
// at all. Confirmed as a real bug via log evidence: with 3 non-minimized
// windows open, only 2 that had actually been focused during this run
// showed up. Windows Polish *does* have recency data for are still
// ordered by true MRU first; anything else falls back to EnumWindows'
// own Z-order (topmost first), appended after.
//
// On a multi-monitor setup, the resulting MRU-then-Z-order list is then
// *grouped* by monitor -- current monitor's windows first (in their
// relative order from that list), then each other monitor's windows in
// turn (see GetMonitorsCurrentFirst) -- per explicit user request: every
// window should still be reachable via Alt+Tab (nothing is excluded),
// but Tab should finish cycling through the current monitor before
// continuing onto the next one, rather than interleaving monitors
// arbitrarily. (An earlier version of this excluded other monitors'
// windows entirely -- reversed after the user clarified they want
// everything reachable, just ordered by monitor.)
void RebuildAltTabCandidates() {
    std::vector<HWND> allCandidates;
    EnumWindows(EnumCandidateWindowsProc, reinterpret_cast<LPARAM>(&allCandidates));
    g_altTabDimTargets = allCandidates;

    std::vector<HWND> globalOrdered;
    for (HWND hwnd : g_activationHistory.OrderedWindows()) {
        if (std::find(allCandidates.begin(), allCandidates.end(), hwnd) != allCandidates.end()) {
            globalOrdered.push_back(hwnd);
        }
    }
    for (HWND hwnd : allCandidates) {
        if (std::find(globalOrdered.begin(), globalOrdered.end(), hwnd) == globalOrdered.end()) {
            globalOrdered.push_back(hwnd);
        }
    }

    g_altTabCandidates.clear();
    for (HMONITOR monitor : GetMonitorsCurrentFirst()) {
        for (HWND hwnd : globalOrdered) {
            if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == monitor) {
                g_altTabCandidates.push_back(hwnd);
            }
        }
    }
}

BOOL CALLBACK EnumMinimizedCandidateWindowsProc(HWND hwnd, LPARAM lParam) {
    if (polish::IsMinimizedCandidateWindow(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

// The minimized-section counterpart of RebuildAltTabCandidates/
// UpdateAltTabCandidatesPreservingOrder -- a single function covers both
// session-start and mid-session refreshes since, unlike the active list,
// there's no order-preservation concern here to justify two separate
// paths (see g_altTabMinimized's own comment).
void RebuildAltTabMinimizedCandidates() {
    g_altTabMinimized.clear();
    EnumWindows(EnumMinimizedCandidateWindowsProc, reinterpret_cast<LPARAM>(&g_altTabMinimized));
}

// The cheap "is a session worth opening at all?" pass, for the one
// caller that runs on the low-level hook thread (AltTabEligibility) and
// so wants an answer without building either real list. Stops at the
// first window of either kind -- active *or* minimized, since a session
// has something to offer either way (see AltTabHasAnythingToShow).
BOOL CALLBACK EnumAnySwitchableWindowProc(HWND hwnd, LPARAM lParam) {
    // IsCandidateWindowShape is exactly "active candidate or minimized
    // candidate" -- the two only split on IsIconic, which doesn't matter
    // here.
    if (polish::IsCandidateWindowShape(hwnd)) {
        *reinterpret_cast<bool*>(lParam) = true;
        return FALSE;  // one is enough -- stop enumerating
    }
    return TRUE;
}

// Whether an open session still has anything to show. Deliberately *not*
// "two or more active windows": a lone active window is still worth a
// session, because its row's minimize/maximize/close buttons act on it
// without switching anywhere, and any minimized window is worth one on
// its own because restoring it is the only way back to it. The single
// case with nothing to offer is a desktop with no windows at all --
// nothing active and nothing minimized.
bool AltTabHasAnythingToShow() {
    return !g_altTabCandidates.empty() || !g_altTabMinimized.empty();
}

// Keeps the invariant the rest of the session code reads off
// g_altTabSelectionInMinimized: selection may only sit in a section that
// actually has rows, and its index must be in range for that section.
// Called after every mid-session rebuild, since either list can empty
// out under an open session (the last active window minimized or
// closed, the last minimized one restored).
void NormalizeAltTabSelectionSection() {
    if (g_altTabCandidates.empty() && !g_altTabMinimized.empty()) {
        g_altTabSelectionInMinimized = true;
    } else if (g_altTabMinimized.empty()) {
        g_altTabSelectionInMinimized = false;
    }
    if (g_altTabSelectionInMinimized) {
        if (g_altTabMinimizedHighlightIndex >= g_altTabMinimized.size()) {
            g_altTabMinimizedHighlightIndex = 0;
        }
    } else if (g_altTabHighlightIndex >= g_altTabCandidates.size()) {
        g_altTabHighlightIndex = 0;
    }
}

// Which monitor a minimized window's row belongs to, grouping it the same
// way BuildAltTabListRowsForMonitor groups active candidates. Deliberately
// NOT MonitorFromWindow(hwnd, ...) -- GetWindowRect (which that resolves
// to under the hood) returns a meaningless off-screen sentinel rect for
// an iconic window, which would make every minimized window resolve to
// whichever monitor happens to contain that sentinel coordinate rather
// than the monitor it actually last sat on. WINDOWPLACEMENT's
// rcNormalPosition is the restore rect, unaffected by the window's
// current iconic state.
HMONITOR MonitorForMinimizedCandidate(HWND hwnd) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement)) {
        return MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
    }
    return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
}

// Used mid-session instead of RebuildAltTabCandidates (which recomputes
// order from scratch every time -- appropriate at session start, but not
// mid-session): keeps every still-open candidate in its *existing*
// g_altTabCandidates position, only dropping ones that closed and
// appending ones that newly appeared. Tab should walk down a list that
// holds still while you're holding Alt, not reshuffle out from under
// you -- confirmed as a real, human-reported problem: RebuildAltTabCandidates'
// MRU-then-Z-order ordering isn't actually stable across a session, since
// ApplyAltTabDimming's own promote-then-demote Z-order pulse on whichever
// window is currently highlighted changes real Z-order every cycle, which
// (for any candidate not yet in g_activationHistory's MRU data) feeds
// straight back into RebuildAltTabCandidates' own "anything else falls
// back to Z-order" tail -- the previously-highlighted window's tail
// position would visibly jump on the very next cycle. Still handles
// windows genuinely opening/closing mid-session (M1's actual goal),
// just without reordering survivors to do it. A brand-new candidate that
// opens mid-session is appended at the very end of the whole list
// (regardless of which monitor it's on) rather than being inserted into
// its "correct" monitor group -- a known, accepted simplification (new
// mid-session candidates are rare, and the existing snapshot's monitor
// grouping is otherwise left completely undisturbed).
void UpdateAltTabCandidatesPreservingOrder() {
    std::vector<HWND> allCandidates;
    EnumWindows(EnumCandidateWindowsProc, reinterpret_cast<LPARAM>(&allCandidates));
    g_altTabDimTargets = allCandidates;

    std::vector<HWND> updated;
    for (HWND hwnd : g_altTabCandidates) {
        if (std::find(allCandidates.begin(), allCandidates.end(), hwnd) != allCandidates.end()) {
            updated.push_back(hwnd);
        }
    }
    // Newly-appeared candidates (opened since the last cycle) -- MRU
    // order first, then Z-order, same append rule RebuildAltTabCandidates
    // itself uses, just restricted to windows `updated` doesn't already
    // have.
    for (HWND hwnd : g_activationHistory.OrderedWindows()) {
        if (std::find(allCandidates.begin(), allCandidates.end(), hwnd) != allCandidates.end() &&
            std::find(updated.begin(), updated.end(), hwnd) == updated.end()) {
            updated.push_back(hwnd);
        }
    }
    for (HWND hwnd : allCandidates) {
        if (std::find(updated.begin(), updated.end(), hwnd) == updated.end()) {
            updated.push_back(hwnd);
        }
    }
    g_altTabCandidates = updated;
}

void EnsureAltTabOverlayPoolSize(size_t count) {
    while (g_altTabOverlays.size() < count) {
        g_altTabOverlays.push_back(std::make_unique<polish::AltTabDimOverlay>(GetModuleHandleW(nullptr)));
    }
}

// Creates group `id`'s own ring on demand, owned by its chrome window --
// see AltTabHighlightBorder's own `owner` comment for what that gets for
// free (always in front of its owner, hidden/shown with minimize/
// restore, destroyed with its owner). One instance per group (not the
// single shared instance an earlier version used) so two groups' rings
// can never fight over one window -- confirmed real: any *other* group's
// reflow used to call Hide() on the one shared ring and silently kill
// whichever group was actually using it.
void EnsureGroupActiveTileHighlight(polish::GroupId id, HWND chromeWindow) {
    if (g_groupActiveTileHighlights.find(id) == g_groupActiveTileHighlights.end()) {
        // alwaysOnTop=false -- unlike the real Alt+Tab overlay, this
        // ring must not float above unrelated windows (e.g. covering
        // VS Code) once the group loses focus; ownership (the `owner`
        // param) keeps it glued to the chrome's own Z position instead.
        g_groupActiveTileHighlights[id] = std::make_unique<polish::AltTabHighlightBorder>(
            GetModuleHandleW(nullptr), /*alwaysOnTop=*/false, chromeWindow);
    }
}

// Shows/hides/repositions the active-tile ring for group `id` to match
// its current state -- called any time something might have changed
// which tile is active, whether it's still Tile mode, where the active
// member's own rect now is, or where the chrome itself now is (a mode
// switch, a reflow, a chrome move, closing the group, ...). Always safe
// to call speculatively; a cheap no-op whenever there's nothing to show.
void UpdateGroupActiveTileHighlight(polish::GroupId id) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto ringIt = g_groupActiveTileHighlights.find(id);
    if (group == nullptr || !polish::IsTiledMode(group->Mode()) || group->MemberCount() <= 1) {
        // A single-member Tile "grid" already fills the whole content
        // area -- nothing to distinguish it from, so no point ringing
        // it.
        if (ringIt != g_groupActiveTileHighlights.end()) {
            ringIt->second->Hide();
        }
        return;
    }
    const auto active = group->ActiveWindow();
    auto chromeIt = g_groupChromeWindows.find(id);
    if (!active.has_value() || !IsWindow(*active) || chromeIt == g_groupChromeWindows.end()) {
        if (ringIt != g_groupActiveTileHighlights.end()) {
            ringIt->second->Hide();
        }
        return;
    }
    EnsureGroupActiveTileHighlight(id, chromeIt->second->Handle());
    g_groupActiveTileHighlights[id]->ShowAroundTarget(*active);
}

// Called when the active tile changes via something other than the
// (Tab-mode-only) tab strip -- specifically, keyboard focus landing
// somewhere inside a Tile-mode member (see OnObjectFocusChanged).
// Mirrors ActivateGroupTab's own GroupState-then-chrome update, minus
// the reflow/refocus steps that only make sense for Tab mode (every
// Tile-mode member is already shown and already has focus -- that's
// the whole reason this fired in the first place).
void ActivateGroupTile(polish::GroupId id, HWND hwnd) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    group->SetActiveWindow(hwnd);
    if (const auto activeIndex = group->ActiveIndex(); activeIndex.has_value()) {
        chromeIt->second->SetActiveIndex(*activeIndex);
    }
    UpdateGroupActiveTileHighlight(id);
}

// Called from GroupChromeWindow's onMemberClicked (WM_PARENTNOTIFY) --
// the click-based counterpart to OnObjectFocusChanged below (see
// SetOnMemberClicked's own comment for why focus events alone aren't
// enough). Resolves the specific member at the click point and
// activates it, same as a focus event landing inside one would.
void OnGroupMemberClicked(polish::GroupId id, POINT clientPt) {
    auto chromeIt = g_groupChromeWindows.find(id);
    if (chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    const HWND chrome = chromeIt->second->Handle();
    const HWND member =
        ChildWindowFromPointEx(chrome, clientPt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
    if (member == nullptr || member == chrome) {
        // No child at that point (only possible if the click didn't
        // actually land on a member -- e.g. a gap between splitters).
        return;
    }
    ActivateGroupTile(id, member);
}

void OnObjectFocusChanged(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd) || g_groupChromeWindows.empty()) {
        return;
    }
    // Walk up from whatever just received focus until either running
    // out of ancestors or finding a window whose *own* parent is a
    // known group chrome -- that window is the specific member (now
    // WS_CHILD) the focus landed inside, however many levels down the
    // actual focused control (an edit control, a list view, ...) sits
    // within that member's own window tree. Bounded by the real (small)
    // depth of a typical window hierarchy, not by anything under this
    // app's control, so no separate iteration cap is needed.
    HWND candidate = hwnd;
    for (HWND parent = GetParent(candidate); parent != nullptr; parent = GetParent(candidate)) {
        for (const auto& [id, chrome] : g_groupChromeWindows) {
            if (chrome->Handle() == parent) {
                ActivateGroupTile(id, candidate);
                return;
            }
        }
        candidate = parent;
    }
}

// Dims every candidate except the highlighted one, in its actual
// on-screen position -- see AltTabDimOverlay.h for why this was chosen
// over a thumbnail-grid popup. The highlighted one also gets a colored
// border frame (AltTabHighlightBorder) so it reads as an active signal
// -- "this one, specifically" -- rather than relying solely on relative
// brightness to notice which window isn't dimmed.
//
// Dimming everything else isn't enough on its own: the highlighted
// window still needs to actually be visible, which it might not be if
// it's currently behind another (e.g. maximized) window. Each dim
// overlay is deliberately NOT topmost (see AltTabDimOverlay.h), so
// they're not the obstacle -- but some other, unrelated window could
// still legitimately be stacked in front of the target on the real
// desktop.
//
// Getting the highlighted window to the front turned out to need a
// specific, well-established technique: promote it to HWND_TOPMOST,
// then *immediately* demote it back to HWND_NOTOPMOST. The brief
// topmost moment forces it above literally everything (every other
// candidate's real window, every dim overlay, any other window on
// screen); the immediate demotion settles it at the very front of the
// normal (non-topmost) band instead of leaving it stuck topmost. This
// also means there's nothing left to clean up in EndAltTabSession --
// the highlighted window is never left topmost even transiently between
// cycles. (An earlier version left it topmost for the duration of being
// highlighted, demoting only when highlight moved away or the session
// ended -- that was still sometimes visually covered by another
// candidate, most likely because SWP_NOACTIVATE alone doesn't reliably
// force DWM to fully recompute z-order for a window that's never
// actually activated. The promote-then-demote pulse sidesteps that
// entirely instead of chasing it further.)
// One monitor's own rows -- the subset of g_altTabCandidates on
// `monitor`, in their existing relative order, plus which local row (if
// any) is the globally-highlighted window (nullopt if the highlighted
// window is on a different monitor's panel instead). Titles/icons are
// re-read fresh from each hwnd every call (not cached) -- cheap for the
// small per-monitor candidate counts this feature deals with, and means
// a title change mid-session shows up without any separate invalidation
// path.
struct MonitorRowsResult {
    std::vector<polish::AltTabListRow> rows;
    std::optional<size_t> highlightIndex;
};

// The single window currently treated as "selected", whether that's the
// normal Tab-cycle highlight (g_altTabHighlightIndex over
// g_altTabCandidates) or, once M4's arrow-navigation has moved into it,
// a row in the minimized section instead (g_altTabMinimizedHighlightIndex
// over g_altTabMinimized) -- see g_altTabSelectionInMinimized. Centralizing
// this one lookup is what lets BuildAltTabListRowsForMonitor mark the
// right row highlighted regardless of which section it's actually in.
HWND CurrentAltTabHighlightedWindow() {
    if (g_altTabSelectionInMinimized) {
        return (g_altTabMinimizedHighlightIndex < g_altTabMinimized.size())
                   ? g_altTabMinimized[g_altTabMinimizedHighlightIndex]
                   : nullptr;
    }
    return (g_altTabHighlightIndex < g_altTabCandidates.size()) ? g_altTabCandidates[g_altTabHighlightIndex]
                                                                 : nullptr;
}

// Called synchronously from inside AltTabHook's low-level hook callback
// (see its class comment for why that's safe here) to decide whether a
// chord should open -- or, for backtick mid-hold, switch -- a session.
//
// Deliberately side-effect free apart from stashing g_pendingTab*: it
// enumerates into locals and never touches g_altTabCandidates or any
// other live session state. That is what makes a mid-hold switch safe --
// the hook must be able to ask "would this give me anything?" without
// clobbering the session currently on screen.
//
// Bounded and UI-free like everything else run from the hook. In
// particular the Tabs answer here is only the cheap half: whether the
// foreground app is one whose tabs can be read at all (an allowlist
// lookup plus one OpenProcess). Actually reading the tabs costs ~50ms of
// cross-process UI Automation and happens on UiaWorker's thread, long
// after this returns -- so Ready here promises a session will be
// attempted, not that it will find enough tabs to paint.
polish::AltTabHook::Eligibility AltTabEligibility(polish::AltTabHook::SessionKind kind) {
    using Eligibility = polish::AltTabHook::Eligibility;
    if (!g_settings.altTabEnabled) {
        return Eligibility::FeatureDisabled;  // keystroke runs untouched -- see AltTabHook
    }

    if (kind == polish::AltTabHook::SessionKind::Windows) {
        bool anything = false;
        EnumWindows(EnumAnySwitchableWindowProc, reinterpret_cast<LPARAM>(&anything));
        return anything ? Eligibility::Ready : Eligibility::NothingToSwitchTo;
    }

    // Tabs. Always the foreground window, even when a window session is
    // already open: Alt+` means "the tabs of the window I am in", and a
    // session only promotes Z-order, it never changes focus, so the
    // foreground window is still the one the user is actually working in.
    const HWND window = GetForegroundWindow();
    if (window == nullptr || g_uiaWorker == nullptr) {
        return Eligibility::NothingToSwitchTo;
    }
    const std::optional<std::wstring> executable = polish::GetWindowProcessImagePath(window);
    if (!executable.has_value()) {
        // Typically an elevated app, whose process this one cannot open --
        // see docs/LIMITATIONS.md #1.
        return Eligibility::NothingToSwitchTo;
    }
    const std::optional<polish::TabRule> rule = polish::FindTabRule(*executable);
    if (!rule.has_value()) {
        return Eligibility::NothingToSwitchTo;  // not allowlisted -- fails closed
    }
    // A snapshot already taken for this same window is authoritative
    // enough to refuse on: if the last read found fewer than two tabs,
    // opening a session would only paint nothing and close again. An
    // unresolved or different-window snapshot is not evidence either way,
    // so those go ahead and let the worker answer properly.
    const polish::UiaWorker::Snapshot snapshot = g_uiaWorker->LatestSnapshot();
    if (snapshot.resolved && snapshot.window == window && snapshot.tabs.size() < polish::kMinimumTabs) {
        return Eligibility::NothingToSwitchTo;
    }
    g_pendingTabWindow = window;
    g_pendingTabRule = *rule;
    return Eligibility::Ready;
}

MonitorRowsResult BuildAltTabListRowsForMonitor(HMONITOR monitor) {
    MonitorRowsResult result;
    const HWND highlighted = CurrentAltTabHighlightedWindow();
    for (HWND hwnd : g_altTabCandidates) {
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != monitor) {
            continue;
        }
        if (hwnd == highlighted) {
            result.highlightIndex = result.rows.size();
        }
        wchar_t title[256] = L"";
        GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        result.rows.push_back(polish::AltTabListRow{hwnd, title, polish::GetWindowIconHandle(hwnd), /*minimized=*/false});
    }
    // Minimized section, appended after every active row so it always
    // renders below them (AltTabListWindow's divider logic assumes
    // minimized rows are contiguous at the end -- see its class comment).
    for (HWND hwnd : g_altTabMinimized) {
        if (MonitorForMinimizedCandidate(hwnd) != monitor) {
            continue;
        }
        if (hwnd == highlighted) {
            result.highlightIndex = result.rows.size();
        }
        wchar_t title[256] = L"";
        GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        result.rows.push_back(polish::AltTabListRow{hwnd, title, polish::GetWindowIconHandle(hwnd), /*minimized=*/true});
    }
    return result;
}

void ApplyAltTabDimming() {
    const ULONGLONG t0 = GetTickCount64();
    // While selection sits in the minimized section, nothing in
    // g_altTabCandidates is "the highlighted one" -- dim all of them (the
    // desktop stays visibly dimmed while browsing the minimized list) and
    // skip the promote/border step entirely below, since a minimized
    // window has no on-screen rect to draw a border around (see
    // PLAN.md's Alt+Tab-improvements M4).
    const bool selectionIsMinimized = g_altTabSelectionInMinimized;
    // Dims g_altTabDimTargets (every window, whatever the scope), not
    // g_altTabCandidates, and finds the highlighted window by identity
    // rather than index -- the two lists only line up under AllWindows.
    //
    // Null whenever there is no on-screen window to highlight: selection
    // in the minimized section, or an active list that is simply empty
    // (a session over nothing but minimized windows -- see
    // AltTabHasAnythingToShow). Everything below keys off the null, not
    // off selectionIsMinimized, so both cases skip the promote/halo step
    // that needs a real rect.
    const HWND highlightedWindow =
        (!selectionIsMinimized && g_altTabHighlightIndex < g_altTabCandidates.size())
            ? g_altTabCandidates[g_altTabHighlightIndex]
            : nullptr;
    for (size_t i = 0; i < g_altTabDimTargets.size(); ++i) {
        HWND hwnd = g_altTabDimTargets[i];
        if (hwnd == highlightedWindow) {
            // Handled last, below -- but clear any dim left over from the
            // previous cycle, when some other window held the highlight.
            g_altTabOverlays[i]->Hide();
            continue;
        }
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_altTabOverlays[i]->ShowOverTarget(hwnd);
    }
    // Overlays past the current target count belong to windows that have
    // since closed (or fallen out of the list); left alone they would sit
    // stuck over whatever real window used to occupy that slot.
    for (size_t i = g_altTabDimTargets.size(); i < g_altTabOverlays.size(); ++i) {
        g_altTabOverlays[i]->Hide();
    }
    const ULONGLONG t1 = GetTickCount64();

    ULONGLONG t2 = t1;
    ULONGLONG t3 = t1;
    if (highlightedWindow != nullptr) {
        const HWND highlighted = highlightedWindow;
        const bool promoted = polish::PromoteWindowToFront(highlighted);
        if (!promoted) {
            // Most likely cause: highlighted belongs to a more-privileged
            // (elevated) process than this one -- UIPI blocks cross-privilege
            // window manipulation. See docs/LIMITATIONS.md #1; this is a
            // known, permanent gap, not something to chase further if that's
            // what the logged error confirms.
            polish::LogDebug(std::format(
                L"[Polish] AltTab: WARNING SetWindowPos(TOPMOST) failed for hwnd={} GetLastError={}",
                reinterpret_cast<void*>(highlighted), GetLastError()));
        }
        t2 = GetTickCount64();

        // Called after PromoteWindowToFront so the halo (HWND_TOP) lands
        // above the just-promoted window and any dim overlays.
        if (g_activeWindowHalo) {
            g_activeWindowHalo->ShowAroundTarget(highlighted);
        }
        t3 = GetTickCount64();
    } else if (g_activeWindowHalo) {
        g_activeWindowHalo->Hide();
    }

    // Timing breadcrumbs to chase a human-reported "flash of the
    // previous window" glitch with real evidence -- a pre-warming fix
    // (creating the overlay/border windows at startup instead of lazily)
    // didn't resolve it, so the cause is still unconfirmed.
    polish::LogDebug(std::format(
        L"[Polish] AltTab: dimming timing -- otherOverlays={}ms highlightPromote={}ms highlightBorder={}ms",
        t1 - t0, t2 - t1, t3 - t2));

    // One list panel per connected monitor, shown alongside the dim/
    // border visuals for every session (not a separately-toggled thing --
    // see PLAN.md's Alt+Tab-improvements plan). Each panel shows only its
    // own monitor's subset of g_altTabCandidates; only the one monitor
    // whose subset actually contains the globally-highlighted window
    // shows a highlighted row.
    //
    // Full rebuild (Show) only when the candidate set itself actually
    // changed since the last cycle; otherwise just move the highlight
    // marker (SetHighlight, a narrow row invalidate). This codebase has
    // hit the "unconditional full repaint causes visible flashing" bug
    // shape three separate times already (the tab strip's hover
    // highlight, a member-title-change repaint, this exact class's own
    // clip-optimization for the highlight border) -- not repeating it in
    // brand new code that already knows better.
    if (!g_altTabPanels.empty()) {
        const bool candidatesChanged = (g_altTabCandidates != g_altTabListWindowLastCandidates) ||
                                        (g_altTabMinimized != g_altTabListWindowLastMinimized);
        if (candidatesChanged) {
            for (auto& panel : g_altTabPanels) {
                panel.window->SetActiveSectionHeader(L"Active");
            }
            std::wstring rowDump;
            for (HWND hwnd : g_altTabCandidates) {
                wchar_t title[128] = L"";
                GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
                if (!rowDump.empty()) {
                    rowDump += L" | ";
                }
                rowDump += std::format(L"{}:\"{}\"", reinterpret_cast<void*>(hwnd), title);
            }
            polish::LogDebug(std::format(L"[Polish] AltTab: list panel rows (highlightIndex={}): {}",
                                          g_altTabHighlightIndex, rowDump));
        }
        for (auto& panel : g_altTabPanels) {
            MonitorRowsResult monitorResult = BuildAltTabListRowsForMonitor(panel.monitor);
            if (monitorResult.rows.empty()) {
                panel.window->Hide();
                continue;
            }
            if (candidatesChanged) {
                panel.window->Show(monitorResult.rows, monitorResult.highlightIndex, panel.monitor);
            } else {
                panel.window->SetHighlight(monitorResult.highlightIndex);
            }
        }
        if (candidatesChanged) {
            g_altTabListWindowLastCandidates = g_altTabCandidates;
            g_altTabListWindowLastMinimized = g_altTabMinimized;
        }
    }
}

void EndAltTabSession() {
    for (auto& overlay : g_altTabOverlays) {
        overlay->Hide();
    }
    if (g_activeWindowHalo) {
        g_activeWindowHalo->Hide();
    }
    for (auto& panel : g_altTabPanels) {
        panel.window->Hide();
    }
    // Forces the next session's first cycle to take ApplyAltTabDimming's
    // full-rebuild (Show) path rather than the cheap SetHighlight-only
    // path, even if the candidate set ends up identical -- SetHighlight
    // never calls ShowWindow, so without this the panel would stay
    // hidden (from the Hide() above) for a whole session that happened
    // to see no candidate-set change since the last one.
    g_altTabListWindowLastCandidates.clear();
    g_altTabListWindowLastMinimized.clear();
    // A fresh session should always start with focus in the active
    // section, never left over in the minimized one from whatever the
    // previous session's arrow-navigation last did.
    g_altTabSelectionInMinimized = false;
    g_altTabMinimizedHighlightIndex = 0;
    g_altTabSessionOpen = false;
    // On Esc-cancel there's no foreground change to otherwise react to,
    // so the halo (hidden for the whole session -- see the
    // g_altTabSessionOpen check at the top of UpdateActiveWindowHalo)
    // would stay hidden indefinitely without this. Harmless to also run
    // on commit (OnAltTabCommit calls EndAltTabSession before its own
    // SetForegroundWindow): this just re-shows the halo on the
    // about-to-be-replaced foreground window for a moment, immediately
    // superseded by the real OnForegroundChanged once the commit's
    // SetForegroundWindow actually lands.
    UpdateActiveWindowHalo(GetForegroundWindow());
}

// --- Alt+` tab sessions -------------------------------------------------
//
// Deliberately a parallel, much smaller path than the window session
// below rather than a generalization of it: exactly one window is
// involved, so there is no per-monitor grouping, no minimized section, no
// MRU order to preserve and no Z-order promotion. The panel is the only
// piece the two genuinely share.
//
// Nothing is dimmed during a tab session, unlike a window one. Every tab
// lives inside the foreground window, which is already covering most of
// the screen -- dimming what little shows around it would darken the
// desktop to say nothing about the choice being made.

// The generation of the enumeration this session is waiting on. Compared
// against what arrives so a result for a superseded request (the user
// Alt+`d in one window, released, and did it again in another) is
// dropped rather than painted over the newer session.
uint64_t g_tabRequestedGeneration = 0;

// Moves `runtimeId` to the front of its window's MRU list. A tab with no
// runtime id (UIA refused to give one) is skipped rather than stored: it
// could not be matched against a later enumeration anyway.
void PromoteTabInMru(HWND window, const std::vector<int>& runtimeId) {
    if (runtimeId.empty()) {
        return;
    }
    std::vector<std::vector<int>>& mru = g_tabMru[window];
    std::erase(mru, runtimeId);
    mru.insert(mru.begin(), runtimeId);
}

// Rebuilds g_tabOrder: the tabs this window has been seen using, most
// recent first, then everything else in the app's own visual order. Also
// drops MRU entries for tabs that have since closed, which is the only
// thing that keeps the list from growing for the life of the process.
void RebuildTabOrder() {
    g_tabOrder.clear();
    g_tabOrder.reserve(g_tabs.size());
    std::vector<std::vector<int>>& mru = g_tabMru[g_tabSessionWindow];
    std::erase_if(mru, [](const std::vector<int>& id) {
        return std::none_of(g_tabs.begin(), g_tabs.end(),
                            [&id](const polish::TabTarget& tab) { return tab.runtimeId == id; });
    });
    for (const std::vector<int>& id : mru) {
        for (size_t i = 0; i < g_tabs.size(); ++i) {
            if (g_tabs[i].runtimeId == id) {
                g_tabOrder.push_back(i);
                break;
            }
        }
    }
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (std::find(g_tabOrder.begin(), g_tabOrder.end(), i) == g_tabOrder.end()) {
            g_tabOrder.push_back(i);
        }
    }
}

void ShowTabPanel() {
    std::vector<polish::AltTabListRow> rows;
    rows.reserve(g_tabOrder.size());
    for (size_t index : g_tabOrder) {
        // No HWND and no icon: a tab is not a window, and the per-row
        // action buttons are switched off below precisely because
        // minimize/maximize/close are meaningless for one.
        rows.push_back(polish::AltTabListRow{nullptr, g_tabs[index].title, nullptr, /*minimized=*/false});
    }
    const HMONITOR monitor = MonitorFromWindow(g_tabSessionWindow, MONITOR_DEFAULTTONEAREST);
    for (auto& panel : g_altTabPanels) {
        if (panel.monitor != monitor) {
            panel.window->Hide();
            continue;
        }
        panel.window->SetActiveSectionHeader(g_tabSessionRule.displayName);
        panel.window->SetRowActionsEnabled(false);
        panel.window->Show(rows, g_tabHighlightIndex, monitor);
    }
    // Forces the next *window* session's first cycle to take the full
    // rebuild path -- see EndAltTabSession for why that matters.
    g_altTabListWindowLastCandidates.clear();
    g_altTabListWindowLastMinimized.clear();
}

void EndTabSession() {
    if (!g_tabSessionOpen) {
        return;
    }
    for (auto& panel : g_altTabPanels) {
        panel.window->Hide();
        // Restored for whatever window session comes next, which does
        // want its rows actionable.
        panel.window->SetRowActionsEnabled(true);
        panel.window->SetActiveSectionHeader(L"Active");
    }
    g_altTabListWindowLastCandidates.clear();
    g_altTabListWindowLastMinimized.clear();
    g_tabSessionOpen = false;
    g_tabsPainted = false;
    g_tabCommitPending = false;
    g_tabs.clear();
    g_tabOrder.clear();
    g_tabHighlightIndex = 0;
    g_tabSessionWindow = nullptr;
    UpdateActiveWindowHalo(GetForegroundWindow());
}

// Activates the highlighted tab and ends the session.
void CommitTabSession() {
    if (!g_tabSessionOpen) {
        return;
    }
    if (!g_tabsPainted) {
        // Alt came back up before the tab list did -- a quick Alt+` tap.
        // Hold the session open; OnTabsReady performs the switch the
        // moment the list lands, so a tap behaves like Alt+Tab's
        // single-tap swap rather than doing nothing at all.
        g_tabCommitPending = true;
        return;
    }
    if (g_tabHighlightIndex >= g_tabOrder.size()) {
        EndTabSession();
        return;
    }
    const size_t workerIndex = g_tabOrder[g_tabHighlightIndex];
    const polish::TabTarget& tab = g_tabs[workerIndex];
    PromoteTabInMru(g_tabSessionWindow, tab.runtimeId);
    if (g_uiaWorker) {
        g_uiaWorker->RequestActivate(g_tabGeneration, workerIndex);
    }
    polish::LogDebug(std::format(L"[Polish] Tabs: commit -> \"{}\" (worker index {})", tab.title, workerIndex));
    EndTabSession();
}

// A tab list has come back from the worker (kTabsReadyMessage).
void OnTabsReady(uint64_t generation) {
    if (!g_tabSessionOpen || g_uiaWorker == nullptr) {
        return;  // session already over -- a prewarm, or an Escape
    }
    if (generation != g_tabRequestedGeneration) {
        return;  // superseded by a newer request
    }
    const polish::UiaWorker::Snapshot snapshot = g_uiaWorker->LatestSnapshot();
    if (snapshot.window != g_tabSessionWindow) {
        return;
    }
    if (snapshot.tabs.size() < polish::kMinimumTabs) {
        polish::LogDebug(std::format(L"[Polish] Tabs: only {} tab(s) -- nothing to switch to, closing session",
                                     snapshot.tabs.size()));
        EndTabSession();
        return;
    }
    g_tabs = snapshot.tabs;
    g_tabGeneration = snapshot.generation;

    // Whatever is frontmost right now is by definition the most recently
    // used tab, however the user got there -- clicking it, Ctrl+Tab, or a
    // previous Alt+`. Promoting it here is what keeps the MRU order
    // honest without watching for tab switches continuously.
    for (const polish::TabTarget& tab : g_tabs) {
        if (tab.selected) {
            PromoteTabInMru(g_tabSessionWindow, tab.runtimeId);
            break;
        }
    }
    RebuildTabOrder();
    // Index 0 is the current tab (freshest in the MRU order); the first
    // press lands on the one used before it, so a quick Alt+` tap toggles
    // between the last two -- exactly how a single Alt+Tab tap swaps to
    // the previous window.
    g_tabHighlightIndex = polish::AdvanceHighlight(0, g_tabOrder.size(), /*backward=*/false);
    g_tabsPainted = true;

    if (g_tabCommitPending) {
        CommitTabSession();
        return;
    }
    ShowTabPanel();
}

// Alt+` pressed -- open a tab session, or advance one already open.
void OnTabCycle(bool backward) {
    if (!g_tabSessionOpen) {
        if (g_pendingTabWindow == nullptr || g_uiaWorker == nullptr) {
            return;
        }
        g_tabSessionWindow = g_pendingTabWindow;
        g_tabSessionRule = g_pendingTabRule;
        g_tabSessionOpen = true;
        g_tabsPainted = false;
        g_tabCommitPending = false;
        g_tabs.clear();
        g_tabHighlightIndex = 0;
        if (g_activeWindowHalo) {
            g_activeWindowHalo->Hide();
            g_haloTarget = nullptr;
        }
        // Nothing is shown yet: reading the tabs takes ~50ms on the
        // worker thread, and OnTabsReady paints when they arrive.
        g_tabRequestedGeneration = g_uiaWorker->RequestTabs(g_tabSessionWindow, g_tabSessionRule);
        polish::LogDebug(std::format(L"[Polish] Tabs: session starting for hwnd={} ({}), awaiting generation {}",
                                     reinterpret_cast<void*>(g_tabSessionWindow), g_tabSessionRule.displayName,
                                     g_tabRequestedGeneration));
        return;
    }
    if (!g_tabsPainted) {
        // Still waiting on the list. Extra presses inside that ~50ms
        // window are dropped rather than queued -- OnTabsReady applies
        // the opening step-of-one regardless, so the session still lands
        // somewhere sensible.
        return;
    }
    g_tabHighlightIndex = polish::AdvanceHighlight(g_tabHighlightIndex, g_tabOrder.size(), backward);
    const HMONITOR monitor = MonitorFromWindow(g_tabSessionWindow, MONITOR_DEFAULTTONEAREST);
    for (auto& panel : g_altTabPanels) {
        if (panel.monitor == monitor) {
            panel.window->SetHighlight(g_tabHighlightIndex);
        }
    }
}

// Both chords land here: Tab with SessionKind::Windows, Alt+` with
// SessionKind::Tabs. The two sessions are mutually exclusive, so whichever
// kind arrives first tears the other down -- that is also what makes the
// hook's mid-hold switching work, since it simply posts the other kind.
void OnAltTabCycle(bool backward, polish::AltTabHook::SessionKind kind) {
    if (kind == polish::AltTabHook::SessionKind::Tabs) {
        if (g_altTabSessionOpen) {
            EndAltTabSession();
        }
        OnTabCycle(backward);
        return;
    }
    if (g_tabSessionOpen) {
        EndTabSession();
    }

    if (g_altTabHook) {
        const ULONGLONG queueDelayMs = GetTickCount64() - g_altTabHook->LastTabDetectedTick();
        polish::LogDebug(
            std::format(L"[Polish] AltTab: message-queue delay since Tab detected: {}ms", queueDelayMs));
    }

    // Capture identity (not index) of the currently highlighted window,
    // and the overlay-relevant size, before rebuilding -- both the
    // candidate's position in the list and the list's own length can
    // change once the rebuild below runs.
    const bool wasSessionOpen = g_altTabSessionOpen;
    const HWND previouslyHighlighted =
        (wasSessionOpen && g_altTabHighlightIndex < g_altTabCandidates.size())
            ? g_altTabCandidates[g_altTabHighlightIndex]
            : nullptr;
    // The minimized section's own anchor, kept separately because the
    // active one above is captured even when selection was sitting in
    // the minimized section (Tab pulls it back to the active list, so
    // the active anchor is the one that matters there). Only used when
    // Tab has to cycle the minimized section itself -- see below.
    const HWND previouslyHighlightedMinimized =
        (wasSessionOpen && g_altTabMinimizedHighlightIndex < g_altTabMinimized.size())
            ? g_altTabMinimized[g_altTabMinimizedHighlightIndex]
            : nullptr;
    // Refreshed on every cycle, not just once at session start, so a
    // window opening/closing/minimizing mid-session (via any means other
    // than this app's own Alt+Tab) is reflected the next time Tab is
    // pressed. Safe to do here: unlike AltTabEligibility, this
    // runs off the hook thread already (dispatched via the PostMessageW
    // hop), so EnumWindows' cost here carries none of the low-level-hook
    // timeout risk that confines hook-thread work to bounded, no-UI
    // calls.
    //
    // Session start uses the full MRU-based RebuildAltTabCandidates (a
    // fresh list is exactly right the first time). Every cycle *after*
    // that uses UpdateAltTabCandidatesPreservingOrder instead -- Tab
    // should walk down a list that holds still while Alt is held, not
    // reshuffle survivors around just because the highlighted window's
    // own Z-order changed (see that function's comment for the concrete
    // mechanism that caused it to visibly reorder before this fix).
    if (wasSessionOpen) {
        UpdateAltTabCandidatesPreservingOrder();
    } else {
        RebuildAltTabCandidates();
    }
    RebuildAltTabMinimizedCandidates();

    // Overlays left over from a longer previous target list are hidden by
    // ApplyAltTabDimming itself, so no shrink handling is needed here.

    if (!AltTabHasAnythingToShow()) {
        // A live rebuild can empty both lists mid-session (the last
        // candidates closing) in a way session start's own eligibility
        // guard can't prevent. End cleanly rather than divide/mod by a
        // degenerate count below.
        if (wasSessionOpen) {
            polish::LogDebug(L"[Polish] AltTab: no windows left at all mid-session, ending session");
            EndAltTabSession();
        }
        return;
    }

    // Tab/Shift+Tab normally operate on the active section, regardless of
    // where arrow-navigation (OnAltTabNavigate) last left selection --
    // see g_altTabSelectionInMinimized's own comment. With nothing active
    // at all, though, the minimized list is the only list there is, so
    // Tab cycles that instead of having nowhere to go: the whole point of
    // opening a session with no active windows is reaching a minimized
    // one.
    const bool cycleMinimized = g_altTabCandidates.empty();
    g_altTabSelectionInMinimized = cycleMinimized;
    std::vector<HWND>& cycleList = cycleMinimized ? g_altTabMinimized : g_altTabCandidates;
    size_t& cycleIndex = cycleMinimized ? g_altTabMinimizedHighlightIndex : g_altTabHighlightIndex;

    EnsureAltTabOverlayPoolSize(g_altTabDimTargets.size());
    const size_t count = cycleList.size();

    if (!wasSessionOpen) {
        std::wstring candidateDump;
        for (HWND hwnd : g_altTabCandidates) {
            wchar_t title[128] = L"";
            GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
            wchar_t className[128] = L"";
            GetClassNameW(hwnd, className, static_cast<int>(sizeof(className) / sizeof(className[0])));
            if (!candidateDump.empty()) {
                candidateDump += L" | ";
            }
            // Class name included specifically to catch a hidden/
            // suspended UWP host window (e.g. ApplicationFrameHost,
            // Windows.UI.Core.CoreWindow) sneaking into the list --
            // human-reported the Settings app sometimes appearing to
            // "launch" mid Alt+Tab when it wasn't running before, most
            // likely explained by committing to one of these instead of
            // a genuinely new process.
            candidateDump += std::format(L"{}:\"{}\"[{}]", reinterpret_cast<void*>(hwnd), title, className);
        }
        if (g_activeWindowHalo) {
            g_activeWindowHalo->Hide();
            g_haloTarget = nullptr;
        }
        polish::LogDebug(std::format(L"[Polish] AltTab: session starting, {} candidate(s): {}",
                                      g_altTabCandidates.size(), candidateDump));
        // Index 0 is the current window itself (freshest in the MRU
        // order); the first Tab press should land on the previous
        // window, matching native Alt+Tab's single-tap-swap behavior.
        // When the minimized list is what's being cycled there is no
        // "current window" sitting at index 0 to skip past, so the first
        // press lands on its first row instead.
        cycleIndex = cycleMinimized ? 0 : (1 % count);
        g_altTabSessionOpen = true;
    } else {
        // A scope change that was triggered while the selection sat in the
        // minimized section keeps it there, on the same window: the
        // anchor is a minimized window, so there is no active-list index
        // to advance from.
        // Re-locate the previously highlighted window by identity -- the
        // rebuild above may have changed its index, or removed it
        // entirely if it closed mid-session (in which case fall back to
        // a clamped index rather than stepping from a stale one). After a
        // scope change the rebuilt list has a different order entirely,
        // so identity is the only thing that carries over; the anchor is
        // guaranteed to be in the new list when narrowing (it defined the
        // scope) and in it when widening (the wider list is a superset).
        size_t baseIndex = std::min(cycleIndex, count - 1);
        const HWND relocate = cycleMinimized ? previouslyHighlightedMinimized : previouslyHighlighted;
        if (relocate) {
            const auto it = std::find(cycleList.begin(), cycleList.end(), relocate);
            if (it != cycleList.end()) {
                baseIndex = static_cast<size_t>(std::distance(cycleList.begin(), it));
            }
        }
        // Advancing on a scope change too, so every Alt+` press means
        // "next window of this app" and every Tab "next window overall",
        // whether it is the first press of a session or a later one.
        cycleIndex = polish::AdvanceHighlight(baseIndex, count, backward);
    }
    ApplyAltTabDimming();
    polish::LogDebug(std::format(L"[Polish] AltTab: cycle {} -> highlighting hwnd={}",
                                  backward ? L"backward" : L"forward",
                                  reinterpret_cast<void*>(cycleList[cycleIndex])));
}

// Windows restricts SetForegroundWindow from background processes (a
// security heuristic against focus-stealing) -- injecting a harmless
// keystroke immediately before the call is a long-established, widely-
// used workaround that resets whatever internal "did this process just
// handle real input" state that heuristic checks. Same technique, same
// reasoning, as AltTabHook's own InjectHarmlessKeystroke (that one
// suppresses a stuck-modifier side effect too, which doesn't apply
// here, so this stays a separate, smaller local helper rather than
// reusing that one). Shared by OnAltTabCommit and ActivateGroupTab,
// below -- both call SetForegroundWindow from this background process.
void InjectHarmlessCtrlKeystroke() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_CONTROL;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void OnAltTabCommit() {
    if (g_tabSessionOpen) {
        CommitTabSession();
        return;
    }
    if (!g_altTabSessionOpen) {
        return;
    }
    const bool selectionIsMinimized = g_altTabSelectionInMinimized;
    const HWND target = CurrentAltTabHighlightedWindow();
    EndAltTabSession();
    if (target != nullptr && IsWindow(target)) {
        if (selectionIsMinimized) {
            // A minimized window needs an explicit restore before
            // SetForegroundWindow reliably brings it to front -- same
            // finding GroupManager already relies on for a maximized/
            // iconic member (see PLAN.md's Alt+Tab-improvements M4).
            ShowWindow(target, SW_RESTORE);
        }
        InjectHarmlessCtrlKeystroke();
        const BOOL result = SetForegroundWindow(target);
        polish::LogDebug(std::format(
            L"[Polish] AltTab: commit -> hwnd={} minimized={} SetForegroundWindow result={} actualForeground={}",
            reinterpret_cast<void*>(target), selectionIsMinimized, result != FALSE,
            reinterpret_cast<void*>(GetForegroundWindow())));
        return;
    }
    polish::LogDebug(std::format(L"[Polish] AltTab: commit -> hwnd={} no longer a valid window",
                                  reinterpret_cast<void*>(target)));
}

void OnAltTabCancel() {
    if (g_tabSessionOpen) {
        // Escape during a tab session, including one still waiting on its
        // list -- EndTabSession clears g_tabCommitPending, so a result
        // that lands afterwards switches nothing.
        EndTabSession();
        polish::LogDebug(L"[Polish] Tabs: cancel");
        return;
    }
    if (!g_altTabSessionOpen) {
        return;
    }
    EndAltTabSession();
    polish::LogDebug(L"[Polish] AltTab: cancel");
}

// Arrow-key navigation, fired only while a session is already active
// (AltTabHook guarantees that -- see its class comment). While selection
// is in the active section, Down/Up behave exactly like Tab/Shift+Tab
// (including wraparound) *except* that Down from the last active row
// moves into the minimized section instead of wrapping to the first
// active row -- Up from the minimized section's first row moves back
// there symmetrically. Once in the minimized section, Down/Up just walk
// it linearly (no wraparound at either end -- there's no "next" list to
// spill into past the last minimized row). Tab/Shift+Tab
// (OnAltTabCycle) always reset out of the minimized section on the very
// next press, regardless of where this last left it.
// How far PageUp/PageDown jumps. A fixed count rather than a real
// viewport-derived page: the candidates are spread across one panel per
// monitor, each with its own height and its own scroll state, so there
// is no single "page" to derive -- and a page that silently meant
// different amounts depending on which monitor the highlight happened to
// be on would be worse than one predictable number.
constexpr size_t kAltTabPageStep = 10;

// Moves the highlight to the first candidate on the next (or previous)
// monitor's panel, skipping panels that currently have no candidates of
// their own. Left/Right mean this, rather than movement within a list,
// because Up/Down already flow between a monitor's Active and Minimized
// sections -- so the horizontal axis is free for the spatially obvious
// meaning on a multi-monitor desktop. A no-op with fewer than two
// panels.
void MoveAltTabHighlightToAdjacentPanel(bool forward) {
    const HWND current = CurrentAltTabHighlightedWindow();
    if (current == nullptr || g_altTabPanels.size() < 2) {
        return;
    }
    const HMONITOR currentMonitor = g_altTabSelectionInMinimized
                                        ? MonitorForMinimizedCandidate(current)
                                        : MonitorFromWindow(current, MONITOR_DEFAULTTONEAREST);
    size_t panelIndex = 0;
    for (size_t i = 0; i < g_altTabPanels.size(); ++i) {
        if (g_altTabPanels[i].monitor == currentMonitor) {
            panelIndex = i;
            break;
        }
    }

    const size_t panelCount = g_altTabPanels.size();
    for (size_t step = 1; step <= panelCount; ++step) {
        const size_t candidatePanel =
            forward ? (panelIndex + step) % panelCount : (panelIndex + panelCount - step) % panelCount;
        const HMONITOR target = g_altTabPanels[candidatePanel].monitor;
        bool landed = false;
        for (size_t i = 0; i < g_altTabCandidates.size(); ++i) {
            if (MonitorFromWindow(g_altTabCandidates[i], MONITOR_DEFAULTTONEAREST) == target) {
                g_altTabHighlightIndex = i;
                g_altTabSelectionInMinimized = false;
                landed = true;
                break;
            }
        }
        // A panel can be showing nothing but minimized rows (its monitor
        // has no active windows, or none of them do) -- landing on its
        // first minimized row is still the move the user asked for, and
        // skipping the panel entirely would make Left/Right dead in a
        // session opened over minimized windows alone.
        if (!landed) {
            for (size_t i = 0; i < g_altTabMinimized.size(); ++i) {
                if (MonitorForMinimizedCandidate(g_altTabMinimized[i]) == target) {
                    g_altTabMinimizedHighlightIndex = i;
                    g_altTabSelectionInMinimized = true;
                    landed = true;
                    break;
                }
            }
        }
        if (landed) {
            ApplyAltTabDimming();
            return;
        }
    }
}

void OnAltTabNavigate(polish::AltTabHook::NavigateStep step) {
    if (!g_altTabSessionOpen) {
        return;
    }
    using Step = polish::AltTabHook::NavigateStep;

    if (step == Step::PrevPanel || step == Step::NextPanel) {
        MoveAltTabHighlightToAdjacentPanel(step == Step::NextPanel);
        return;
    }

    // Home/End always mean the active section's own ends -- jumping into
    // the minimized section is Down's job, and an "End" that could land
    // in either section depending on prior state would be unpredictable.
    if (step == Step::First || step == Step::Last) {
        if (g_altTabCandidates.empty()) {
            // No active section to jump within -- with only minimized
            // windows on screen, its ends are the only ones there are.
            if (g_altTabMinimized.empty()) {
                return;
            }
            g_altTabSelectionInMinimized = true;
            g_altTabMinimizedHighlightIndex = (step == Step::First) ? 0 : g_altTabMinimized.size() - 1;
            ApplyAltTabDimming();
            return;
        }
        g_altTabSelectionInMinimized = false;
        g_altTabHighlightIndex = (step == Step::First) ? 0 : g_altTabCandidates.size() - 1;
        ApplyAltTabDimming();
        return;
    }

    // Paging clamps at both ends rather than wrapping (unlike Tab's own
    // cycle, which deliberately wraps): paging is for covering a long
    // list quickly, and wrapping past the end there tends to lose the
    // user's place rather than help.
    if (step == Step::PageUp || step == Step::PageDown) {
        if (g_altTabSelectionInMinimized) {
            RebuildAltTabMinimizedCandidates();
            if (g_altTabMinimized.empty()) {
                g_altTabSelectionInMinimized = false;
            } else {
                g_altTabMinimizedHighlightIndex =
                    (step == Step::PageDown)
                        ? std::min(g_altTabMinimizedHighlightIndex + kAltTabPageStep, g_altTabMinimized.size() - 1)
                        : (g_altTabMinimizedHighlightIndex < kAltTabPageStep
                               ? 0
                               : g_altTabMinimizedHighlightIndex - kAltTabPageStep);
                ApplyAltTabDimming();
                return;
            }
        }
        if (g_altTabCandidates.empty()) {
            return;
        }
        g_altTabHighlightIndex =
            (step == Step::PageDown)
                ? std::min(g_altTabHighlightIndex + kAltTabPageStep, g_altTabCandidates.size() - 1)
                : (g_altTabHighlightIndex < kAltTabPageStep ? 0 : g_altTabHighlightIndex - kAltTabPageStep);
        ApplyAltTabDimming();
        return;
    }

    const bool downward = (step == Step::Next);
    if (!g_altTabSelectionInMinimized && downward && !g_altTabCandidates.empty() &&
        g_altTabHighlightIndex + 1 >= g_altTabCandidates.size()) {
        RebuildAltTabMinimizedCandidates();
        if (!g_altTabMinimized.empty()) {
            g_altTabSelectionInMinimized = true;
            g_altTabMinimizedHighlightIndex = 0;
            ApplyAltTabDimming();
            return;
        }
        // Nothing minimized to move into -- fall through to the normal
        // wraparound cycle below, same as if this check never fired.
    }

    if (g_altTabSelectionInMinimized) {
        RebuildAltTabMinimizedCandidates();
        if (g_altTabMinimized.empty()) {
            g_altTabSelectionInMinimized = false;
        } else if (downward) {
            if (g_altTabMinimizedHighlightIndex + 1 < g_altTabMinimized.size()) {
                ++g_altTabMinimizedHighlightIndex;
            }
        } else if (g_altTabMinimizedHighlightIndex == 0) {
            // Up off the top of the minimized section moves back into the
            // active one -- unless there isn't one, in which case this is
            // already the first row on screen and Up just stays put.
            if (!g_altTabCandidates.empty()) {
                g_altTabSelectionInMinimized = false;
            }
        } else {
            --g_altTabMinimizedHighlightIndex;
        }
        NormalizeAltTabSelectionSection();
        ApplyAltTabDimming();
        return;
    }

    // Same scope as the session already has: a Tab-key-free step through
    // the active list (Up/Down running off the minimized section's top)
    // must not change what the session is scoped to.
    OnAltTabCycle(/*backward=*/!downward, polish::AltTabHook::SessionKind::Windows);
}

// A row click in the list panel commits directly to that row's window,
// regardless of whichever one Tab-cycling last landed the highlight on.
// Takes the clicked row's own HWND (not an index) -- each monitor's panel
// only knows its own local subset, so a plain index would be ambiguous
// without knowing which panel it came from; looking the HWND up here
// resolves it to the right position in the one shared g_altTabCandidates
// list.
void OnAltTabRowActivated(HWND hwnd) {
    if (!g_altTabSessionOpen) {
        return;
    }
    const auto activeIt = std::find(g_altTabCandidates.begin(), g_altTabCandidates.end(), hwnd);
    if (activeIt != g_altTabCandidates.end()) {
        g_altTabHighlightIndex = static_cast<size_t>(std::distance(g_altTabCandidates.begin(), activeIt));
        g_altTabSelectionInMinimized = false;
        OnAltTabCommit();
        return;
    }
    // Not an active candidate -- check the minimized section (see
    // PLAN.md's Alt+Tab-improvements M4). A click there should restore
    // and commit directly, same as Enter after arrow-navigating to it.
    const auto minimizedIt = std::find(g_altTabMinimized.begin(), g_altTabMinimized.end(), hwnd);
    if (minimizedIt != g_altTabMinimized.end()) {
        g_altTabMinimizedHighlightIndex = static_cast<size_t>(std::distance(g_altTabMinimized.begin(), minimizedIt));
        g_altTabSelectionInMinimized = true;
        OnAltTabCommit();
    }
}

// Fired by AltTabListWindow's minimize/restore-toggle button, which only
// ever appears on the highlighted row or a merely-hovered row (see
// PLAN.md's Alt+Tab-improvements M5) -- hwnd may therefore be some row
// other than whatever Tab-cycling last highlighted. Acts on it directly
// without ending the session, then rebuilds both lists and re-locates
// the highlight to hwnd by identity, same shape as OnAltTabCycle's own
// mid-session refresh -- toggling naturally moves hwnd to the other
// section, so this is what makes the highlight jump to follow it there
// (whether or not hwnd was already the highlighted row) rather than
// landing on whatever now occupies its old slot.
void OnAltTabRowMinimizeToggle(HWND hwnd) {
    if (!g_altTabSessionOpen || !IsWindow(hwnd)) {
        return;
    }
    // Where the window sat in the active list before it moved, so the
    // selection can stay put rather than follow it out. See below.
    const bool minimizing = !IsIconic(hwnd);
    size_t activeIndexBefore = 0;
    bool hadActiveIndex = false;
    if (minimizing) {
        const auto before = std::find(g_altTabCandidates.begin(), g_altTabCandidates.end(), hwnd);
        if (before != g_altTabCandidates.end()) {
            activeIndexBefore = static_cast<size_t>(std::distance(g_altTabCandidates.begin(), before));
            hadActiveIndex = true;
        }
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_MINIMIZE);
    }

    UpdateAltTabCandidatesPreservingOrder();
    RebuildAltTabMinimizedCandidates();
    if (!AltTabHasAnythingToShow()) {
        // Same empty-desktop guard as OnAltTabCycle. Minimizing can't
        // actually reach it (the window just moves to the other list),
        // but the rebuild above also picks up windows closed by other
        // means since the last refresh, which can.
        polish::LogDebug(L"[Polish] AltTab: no windows left at all after row minimize toggle, ending session");
        EndAltTabSession();
        return;
    }
    EnsureAltTabOverlayPoolSize(g_altTabDimTargets.size());

    const auto activeIt = std::find(g_altTabCandidates.begin(), g_altTabCandidates.end(), hwnd);
    if (activeIt != g_altTabCandidates.end()) {
        // Restored: it has just joined the active list, and following it
        // there is the point of having pressed restore.
        g_altTabHighlightIndex = static_cast<size_t>(std::distance(g_altTabCandidates.begin(), activeIt));
        g_altTabSelectionInMinimized = false;
    } else if (minimizing && hadActiveIndex && !g_altTabCandidates.empty()) {
        // Minimized, so it has left the active list. Stay in that list and
        // land on whichever window took its place -- minimizing is
        // normally one step of clearing several windows out of the way,
        // and following this one down into the Minimized section would
        // interrupt that every time. Same rule the group picker uses when
        // a run of windows is moved between its two lists.
        //
        // Clamped because the window may have been last, in which case
        // there is no row in its old position to take.
        g_altTabHighlightIndex = std::min(activeIndexBefore, g_altTabCandidates.size() - 1);
        g_altTabSelectionInMinimized = false;
    } else {
        // Also reached when hwnd was the *last* active window: there is
        // no active row left to stay on, so the selection follows it down
        // into the minimized section rather than pointing at nothing.
        const auto minimizedIt = std::find(g_altTabMinimized.begin(), g_altTabMinimized.end(), hwnd);
        if (minimizedIt != g_altTabMinimized.end()) {
            g_altTabMinimizedHighlightIndex = static_cast<size_t>(std::distance(g_altTabMinimized.begin(), minimizedIt));
            g_altTabSelectionInMinimized = true;
        } else if (!g_altTabCandidates.empty()) {
            // hwnd vanished entirely (closed itself in response, or some
            // other race) -- clamp rather than reference a stale index.
            g_altTabHighlightIndex = std::min(g_altTabHighlightIndex, g_altTabCandidates.size() - 1);
            g_altTabSelectionInMinimized = false;
        }
    }
    NormalizeAltTabSelectionSection();
    ApplyAltTabDimming();
}

// Fired by AltTabListWindow's maximize/restore-toggle button, which only
// ever appears on an active-section row (see PLAN.md's Alt+Tab-
// improvements M5 follow-up) -- unlike minimize/close, maximize/restore
// never moves hwnd between sections, so this doesn't need to relocate
// anything, just refresh so the highlight border immediately matches the
// window's new (restored/maximized) bounds instead of waiting for the
// next Tab press. Deliberately a no-op for a minimized-section hwnd
// (including one reached via the Plus keyboard shortcut while browsing
// that section) -- maximizing only ever applied to "active windows" per
// the feature request, not as a side-channel way to un-minimize.
void OnAltTabRowMaximizeToggle(HWND hwnd) {
    if (!g_altTabSessionOpen || hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }
    const auto it = std::find(g_altTabCandidates.begin(), g_altTabCandidates.end(), hwnd);
    if (it == g_altTabCandidates.end()) {
        return;
    }
    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_MAXIMIZE);
    }
    g_altTabHighlightIndex = static_cast<size_t>(std::distance(g_altTabCandidates.begin(), it));
    g_altTabSelectionInMinimized = false;
    ApplyAltTabDimming();
    // ApplyAltTabDimming's own panel-refresh only repaints when the
    // candidate list or highlight index actually changed -- neither did
    // here (hwnd stayed in the same slot), so without this the row's
    // maximize/restore glyph would keep showing its pre-toggle state
    // until some other, unrelated change happened to trigger a repaint.
    // Confirmed as a real, human-reported bug: the icon didn't update
    // immediately after clicking it.
    for (auto& panel : g_altTabPanels) {
        panel.window->RepaintRow(hwnd);
    }
}

// Fired by AltTabListWindow's close ("X") button on the highlighted row
// or, per explicit user request, any merely-hovered row too. Posts a
// graceful WM_CLOSE the target app can still intercept (an unsaved-
// changes prompt, etc.), deliberately not DestroyWindow -- but rather
// than waiting for that asynchronous close to actually take effect, this
// removes hwnd from whichever list it's in immediately and fixes up the
// highlight right away: if hwnd was the currently-selected window, the
// selection advances to what's now "next" (the row that shifted into its
// old slot); otherwise the highlight index is just adjusted to keep
// pointing at the same logical window it already did. Waiting for the
// natural next refresh instead was confirmed as feeling broken, human-
// reported, on a window that's slow to actually close. If the app
// cancels the close (its own unsaved-changes prompt, say), the window
// simply reappears -- appended at the end -- on the very next Tab press,
// via UpdateAltTabCandidatesPreservingOrder's normal "newly seen"
// handling; an accepted, minor simplification rather than tracking
// pending closes explicitly.
void OnAltTabRowClose(HWND hwnd) {
    if (!g_altTabSessionOpen || !IsWindow(hwnd)) {
        return;
    }
    polish::LogDebug(std::format(L"[Polish] AltTab: row close -> hwnd={}", reinterpret_cast<void*>(hwnd)));
    PostMessageW(hwnd, WM_CLOSE, 0, 0);

    const auto activeIt = std::find(g_altTabCandidates.begin(), g_altTabCandidates.end(), hwnd);
    if (activeIt != g_altTabCandidates.end()) {
        const size_t removedIndex = static_cast<size_t>(std::distance(g_altTabCandidates.begin(), activeIt));
        g_altTabCandidates.erase(activeIt);
        // Also drop it from the dim targets: it is on its way out, and
        // leaving it there would keep dimming a closing window.
        g_altTabDimTargets.erase(std::remove(g_altTabDimTargets.begin(), g_altTabDimTargets.end(), hwnd),
                                 g_altTabDimTargets.end());
        if (!AltTabHasAnythingToShow()) {
            polish::LogDebug(L"[Polish] AltTab: no windows left at all after row close, ending session");
            EndAltTabSession();
            return;
        }
        if (g_altTabCandidates.empty()) {
            // That was the last active window, but minimized ones remain
            // -- the session stays open over those, with selection moved
            // into the only section still holding rows.
            g_altTabSelectionInMinimized = true;
            g_altTabMinimizedHighlightIndex = 0;
        } else if (removedIndex == g_altTabHighlightIndex) {
            g_altTabHighlightIndex = removedIndex % g_altTabCandidates.size();
        } else if (removedIndex < g_altTabHighlightIndex) {
            --g_altTabHighlightIndex;
        }
        ApplyAltTabDimming();
        return;
    }

    const auto minimizedIt = std::find(g_altTabMinimized.begin(), g_altTabMinimized.end(), hwnd);
    if (minimizedIt != g_altTabMinimized.end()) {
        const size_t removedIndex = static_cast<size_t>(std::distance(g_altTabMinimized.begin(), minimizedIt));
        g_altTabMinimized.erase(minimizedIt);
        if (!AltTabHasAnythingToShow()) {
            polish::LogDebug(L"[Polish] AltTab: no windows left at all after row close, ending session");
            EndAltTabSession();
            return;
        }
        if (g_altTabMinimized.empty()) {
            g_altTabSelectionInMinimized = false;
        } else if (removedIndex == g_altTabMinimizedHighlightIndex) {
            g_altTabMinimizedHighlightIndex = removedIndex % g_altTabMinimized.size();
        } else if (removedIndex < g_altTabMinimizedHighlightIndex) {
            --g_altTabMinimizedHighlightIndex;
        }
        ApplyAltTabDimming();
    }
}

// (Re)builds g_altTabPanels from the currently-connected monitors --
// called once at startup, and again on WM_DISPLAYCHANGE (a monitor
// connected/disconnected, or a resolution/topology change). Without the
// WM_DISPLAYCHANGE call, the panels created at startup would keep
// referencing whichever monitors were connected *then*: confirmed as a
// real bug, human-reported -- disconnecting a second monitor without
// restarting Polish left every panel silently showing nothing at all,
// since none of the (now stale/nonexistent) cached HMONITOR handles
// matched any current window's real monitor anymore, and
// BuildAltTabListRowsForMonitor's per-panel row list came back empty for
// every panel. Destroying and recreating the whole set (rather than
// trying to diff old vs. new monitors) keeps this simple -- monitor
// topology changes are rare and never happen mid-Alt+Tab-session in
// practice (ends any open session first regardless, to be safe if one
// somehow is).
void RefreshAltTabPanels() {
    if (g_altTabSessionOpen) {
        EndAltTabSession();
    }
    g_altTabPanels.clear();
    for (HMONITOR monitor : GetMonitorsCurrentFirst()) {
        AltTabMonitorPanel panel{monitor, std::make_unique<polish::AltTabListWindow>(GetModuleHandleW(nullptr))};
        panel.window->SetOnRowActivated(OnAltTabRowActivated);
        panel.window->SetOnRowMinimizeToggle(OnAltTabRowMinimizeToggle);
        panel.window->SetOnRowMaximizeToggle(OnAltTabRowMaximizeToggle);
        panel.window->SetOnRowClose(OnAltTabRowClose);
        g_altTabPanels.push_back(std::move(panel));
    }
    polish::LogDebug(std::format(L"[Polish] AltTab: panels rebuilt for {} monitor(s)", g_altTabPanels.size()));
}

constexpr UINT kMenuIdRestoreSync = 1;
constexpr UINT kMenuIdAltTab = 2;
constexpr UINT kMenuIdNewGroup = 3;
constexpr UINT kMenuIdChangeGroupHotkey = 4;
constexpr UINT kMenuIdStartAtLogin = 5;
constexpr UINT kMenuIdAbout = 6;
constexpr UINT kMenuIdExit = 7;
constexpr UINT kMenuIdHalo = 8;
constexpr UINT kMenuIdBullseye = 9;

constexpr wchar_t kAboutUrl[] = L"https://www.linkedin.com/in/davidlenihan/";

// Global hotkey id for RegisterHotKey/WM_HOTKEY. No existing hotkey
// infrastructure to reuse (the old HotkeyManager was deleted earlier in
// this project's history); a single RegisterHotKey call is simple
// enough not to need one of its own, unlike Alt+Tab's WH_KEYBOARD_LL
// hook (there's no native OS behavior to suppress here, just one
// combination to claim). The actual combination is user-configurable
// (Settings::groupHotkeyModifiers/groupHotkeyVirtualKey, default
// Win+Alt+G) -- confirmed necessary, not just nice-to-have: something
// else on the dev machine itself already claims Win+Alt+G.
constexpr int kNewGroupHotkeyId = 1;

// (Re-)registers the group hotkey from g_settings' current combination,
// unregistering any previous one first (harmless no-op if none was
// registered). Returns whether registration succeeded. Called at
// startup and again from ChangeGroupHotkey after the user picks a new
// combination.
bool RegisterGroupHotkeyFromSettings() {
    UnregisterHotKey(g_messageWindow, kNewGroupHotkeyId);
    return RegisterHotKey(g_messageWindow, kNewGroupHotkeyId, g_settings.groupHotkeyModifiers | MOD_NOREPEAT,
                           g_settings.groupHotkeyVirtualKey) != FALSE;
}

// Formats a hotkey combination the way the tray menu / dialogs show it,
// e.g. "Win+Alt+G".
std::wstring FormatHotkey(UINT modifiers, UINT virtualKey) {
    std::wstring text;
    if (modifiers & MOD_WIN) text += L"Win+";
    if (modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (modifiers & MOD_ALT) text += L"Alt+";
    if (modifiers & MOD_SHIFT) text += L"Shift+";
    text += static_cast<wchar_t>(virtualKey);
    return text;
}

// Triggered by the tray menu's "Change Group Hotkey..." item: shows
// GroupHotkeyDialog pre-filled with the current combination, and on
// Save actually attempts to register it -- a combination can be
// syntactically valid (the dialog's own job) but still already claimed
// by something else on the machine (confirmed real, see
// kNewGroupHotkeyId's comment), so this loops back to the same dialog
// with an inline error instead of silently leaving no hotkey active.
void ChangeGroupHotkey() {
    polish::HotkeyChoice current{g_settings.groupHotkeyModifiers, g_settings.groupHotkeyVirtualKey};
    for (;;) {
        polish::GroupHotkeyDialog dialog(GetModuleHandleW(nullptr));
        const auto choice = dialog.ShowModal(nullptr, current);
        if (!choice.has_value()) {
            polish::LogDebug(L"[Polish] ChangeGroupHotkey: cancelled");
            return;
        }

        const UINT previousModifiers = g_settings.groupHotkeyModifiers;
        const UINT previousVirtualKey = g_settings.groupHotkeyVirtualKey;
        g_settings.groupHotkeyModifiers = choice->modifiers;
        g_settings.groupHotkeyVirtualKey = choice->virtualKey;
        if (RegisterGroupHotkeyFromSettings()) {
            polish::SaveSettings(g_settings);
            polish::LogDebug(std::format(L"[Polish] ChangeGroupHotkey: now {}",
                                          FormatHotkey(choice->modifiers, choice->virtualKey)));
            return;
        }

        // Failed -- most likely already claimed by something else.
        // Restore the previous combination (so the app isn't left with
        // no working hotkey at all) and let the user try again.
        g_settings.groupHotkeyModifiers = previousModifiers;
        g_settings.groupHotkeyVirtualKey = previousVirtualKey;
        RegisterGroupHotkeyFromSettings();
        polish::LogDebug(std::format(L"[Polish] ChangeGroupHotkey: {} is already in use, GetLastError={}",
                                      FormatHotkey(choice->modifiers, choice->virtualKey), GetLastError()));
        MessageBoxW(nullptr,
                    std::format(L"{} is already in use by another program on this PC. Pick a different combination.",
                                FormatHotkey(choice->modifiers, choice->virtualKey))
                        .c_str(),
                    L"Hotkey unavailable", MB_OK | MB_ICONWARNING);
        current = *choice;  // keep what they typed, let them adjust it
    }
}

// True only while a ReflowGroupTo call is actively running its own
// GrowContentAreaTo step -- guards against the reentrant onResized_
// callback that step itself triggers (see below).
bool g_reflowGrowInProgress = false;

// Fired by kThumbnailRefreshTimerId, some time after the last reflow --
// re-captures every currently-hidden member's thumbnail across every
// group (GroupManager::RefreshThumbnail already no-ops for a
// visible/active member, so this is cheap/safe to call broadly rather
// than needing to track exactly which group/members just changed).
void RefreshAllHiddenThumbnails() {
    for (const polish::GroupState& group : g_groupManager.Groups()) {
        for (const polish::GroupMember& member : group.Members()) {
            if (member.kind == polish::GroupMemberKind::Window && member.window != nullptr) {
                g_groupManager.RefreshThumbnail(member.window);
            }
        }
    }
}

// Reparents (if not already) and positions/shows group id's members --
// the single path both the initial layout (TriggerNewGroup) and every
// later reflow (a tab click, a resize, an edit) go through, so they can
// never drift out of sync with each other. Takes no rect -- members are
// real children of the chrome now, so this always just asks the chrome
// for its own current content area (client-relative coordinates) rather
// than being handed one; a plain drag doesn't even need to call this at
// all any more (children move for free with their parent).
//
// If ApplyLayout reports that some member wouldn't fit -- a real,
// confirmed case: a window's own declared minimum size (Outlook is a
// real example a user hit) can be larger than the content area, and
// SetWindowPos silently clamps to it rather than failing -- the chrome
// is grown to fit and layout is re-applied once, so that member ends up
// correctly filling its (now larger) share of the group instead of
// visibly spilling outside it.
void ReflowGroupTo(polish::GroupId id) {
    if (g_reflowGrowInProgress) {
        // GrowContentAreaTo's own SetWindowPos call below fires WM_SIZE
        // synchronously, which re-enters here via GroupChromeWindow's
        // onResized_ callback *before* the outer call below has made
        // its own follow-up ApplyLayout call. Without this guard, that
        // reentrant call would run its own layout pass (against a
        // content rect that's still momentarily out of sync with what
        // the outer call is about to apply), and if its own measurement
        // differs by even a little, grow again, re-entering again -- a
        // cascade of resize/show/hide cycles a real user saw as visible
        // window flashing. The outer call always finishes the job
        // itself right after GrowContentAreaTo returns, so a reentrant
        // call here has nothing useful left to do.
        return;
    }
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    // A member's position is fully owned by GroupManager now -- but
    // nothing else proactively notices when a window that restore-sync
    // was already tracking (g_trackedWindow, from before it joined a
    // group) becomes a member, since reparenting doesn't fire
    // EVENT_SYSTEM_FOREGROUND. Left alone, every position change
    // GroupManager makes to that member keeps re-triggering restore-
    // sync's own settle-and-SetWindowPlacement logic on it -- two
    // systems fighting over the same window's position, which is
    // exactly what a real, confirmed flashing report traced back to.
    // Cleared here (same reset as EVENT_OBJECT_DESTROY's cleanup)
    // rather than only in OnForegroundChanged, since that path is never
    // reached for this transition at all.
    //
    // This must run *before* ApplyLayout below, not after: membership
    // itself doesn't depend on ApplyLayout having run (CreateGroup/
    // SetMembers already populate it), but ApplyLayout is what actually
    // reparents/repositions a brand-new member for the first time --
    // and if that member happened to be g_trackedWindow (e.g. it was
    // the foreground window when "New Group" was triggered, a common
    // real case), its own EnsureReparented/PositionMember calls could
    // fire the very location-change events this clear is meant to
    // guard against, in the gap where tracking was still live. Clearing
    // first closes that race regardless of how those events end up
    // getting delivered.
    if (g_trackedWindow != nullptr && group->Contains(g_trackedWindow)) {
        g_trackedWindow = nullptr;
        g_inMoveSizeLoop = false;
        g_pendingSettleRect.reset();
        KillTimer(g_messageWindow, kSettleTimerId);
    }

    const HWND chromeHandle = chromeIt->second->Handle();
    const RECT contentRect = chromeIt->second->ContentRectInClientCoords();
    const size_t memberCountBefore = group->MemberCount();
    const SIZE needed =
        g_groupManager.ApplyLayout(*group, chromeHandle, contentRect, chromeIt->second->TileSplitterWidthPx());
    // ApplyLayout can drop a member that turned out to be unreparentable
    // (see its own comment) -- when it does, the chrome's tab strip
    // still shows the stale, now-too-long title/icon list until
    // something resyncs it, so do that here rather than leaving a
    // ghost tab behind.
    if (group->MemberCount() != memberCountBefore) {
        chromeIt->second->SetMemberTitles(CollectMemberTitles(*group));
        chromeIt->second->SetMemberIcons(CollectMemberIcons(*group));
        if (const auto activeIndex = group->ActiveIndex(); activeIndex.has_value()) {
            chromeIt->second->SetActiveIndex(*activeIndex);
        }
    }
    // A no-op in Tab mode (both come back empty) -- ApplyLayout is what
    // actually (re)computes these, so the chrome's own copy (used for
    // splitter rendering/hit-testing) needs refreshing after every call
    // to it, not just the first.
    chromeIt->second->SetTileSplitters(g_groupManager.TileColumnBoundaries(id), g_groupManager.TileRowBoundaries(id));

    // Re-arm (not just start) the delayed thumbnail-refresh sweep on
    // every reflow -- see kThumbnailRefreshTimerId's own comment for
    // why a single synchronous capture isn't always enough.
    SetTimer(g_messageWindow, kThumbnailRefreshTimerId, kThumbnailRefreshDelayMs, nullptr);

    const int requestedWidth = contentRect.right - contentRect.left;
    const int requestedHeight = contentRect.bottom - contentRect.top;
    if (needed.cx > requestedWidth || needed.cy > requestedHeight) {
        polish::LogDebug(std::format(
            L"[Polish] Group: member(s) wouldn't fit group id={} (requested {}x{}, needed {}x{}) -- growing chrome",
            id, requestedWidth, requestedHeight, needed.cx, needed.cy));
        g_reflowGrowInProgress = true;
        chromeIt->second->GrowContentAreaTo(needed);
        g_reflowGrowInProgress = false;
        g_groupManager.ApplyLayout(*group, chromeHandle, chromeIt->second->ContentRectInClientCoords(),
                                    chromeIt->second->TileSplitterWidthPx());
        chromeIt->second->SetTileSplitters(g_groupManager.TileColumnBoundaries(id),
                                            g_groupManager.TileRowBoundaries(id));
    }

    // The active member's screen rect may have just moved (a new tile
    // grid shape, a resize, ...) -- keep the active-tile ring (if
    // showing at all) glued to it. A cheap no-op when there's nothing
    // to update (wrong mode, ≤1 member, etc. -- see its own comment).
    UpdateGroupActiveTileHighlight(id);
}

// Called live while a Tile-mode splitter is being dragged
// (GroupChromeWindow's onTileSplitterDragged callback, already clamped
// there so neither adjacent tile shrinks below its visible-content
// floor) -- updates just that one pair of adjacent tiles' stored
// fractions and reflows, so the drag visibly resizes tiles in real
// time rather than only once on drop.
void OnTileSplitterDragged(polish::GroupId id, bool column, size_t index, int newPixelPosition) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    const RECT contentRect = chromeIt->second->ContentRectInClientCoords();
    const int totalSize = column ? (contentRect.right - contentRect.left) : (contentRect.bottom - contentRect.top);
    g_groupManager.SetTileBoundary(*group, column, index, newPixelPosition, totalSize,
                                    chromeIt->second->TileSplitterWidthPx());
    ReflowGroupTo(id);
}

// Remembers each splitter's most recent *custom* (non-50/50) fraction
// pair, keyed by (group, axis, index) -- so a double-click can toggle
// back to it after having snapped to an even split. Only ever holds an
// entry while that splitter is currently sitting at 50/50 because of a
// double-click; a live drag (OnTileSplitterDragged) doesn't touch this
// map at all, so dragging away from an even split simply leaves no
// stale entry to toggle back to (there's nothing to "undo" yet).
std::map<std::tuple<polish::GroupId, bool, size_t>, std::pair<double, double>> g_tileSplitterLastCustom;

// Called when a Tile-mode splitter is double-clicked
// (GroupChromeWindow's onTileSplitterDoubleClicked callback) -- toggles
// that one pair of adjacent tiles between an even 50/50 split and
// whatever custom split they had before, so a quick double-click undoes
// a drag without having to eyeball it back into place.
void OnTileSplitterDoubleClicked(polish::GroupId id, bool column, size_t index) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    if (group == nullptr) {
        return;
    }
    std::vector<double> fractions = column ? group->TileColumnFractions() : group->TileRowFractions();
    if (index + 1 >= fractions.size()) {
        return;
    }

    const double pairTotal = fractions[index] + fractions[index + 1];
    const double evenSplit = pairTotal / 2.0;
    constexpr double kEvenTolerance = 0.005;  // ~0.5% of the pair -- comfortably tighter than any visible difference
    const auto key = std::make_tuple(id, column, index);

    if (std::abs(fractions[index] - evenSplit) < kEvenTolerance) {
        // Already even -- toggle back to the last custom split, if any.
        const auto it = g_tileSplitterLastCustom.find(key);
        if (it == g_tileSplitterLastCustom.end()) {
            return;  // nothing to restore
        }
        fractions[index] = it->second.first;
        fractions[index + 1] = it->second.second;
        g_tileSplitterLastCustom.erase(it);
    } else {
        // Currently custom -- remember it, then snap to even.
        g_tileSplitterLastCustom[key] = {fractions[index], fractions[index + 1]};
        fractions[index] = evenSplit;
        fractions[index + 1] = evenSplit;
    }

    if (column) {
        group->SetTileColumnFractions(std::move(fractions));
    } else {
        group->SetTileRowFractions(std::move(fractions));
    }
    ReflowGroupTo(id);
}

// Current window titles for group's members, in membership order --
// shared by the initial chrome creation, a live title-change sync, a
// tab reorder, and an edit-membership confirm, so the chrome's tab
// labels are always rebuilt the same way regardless of what triggered
// the refresh.
std::vector<std::wstring> CollectMemberTitles(const polish::GroupState& group) {
    std::vector<std::wstring> titles;
    for (const polish::GroupMember& member : group.Members()) {
        if (member.kind != polish::GroupMemberKind::Window || member.window == nullptr) {
            continue;  // nested-group case -- v1 never populates this
        }
        wchar_t title[256] = L"";
        GetWindowTextW(member.window, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        titles.emplace_back(title);
    }
    return titles;
}

// Current member icons, in the same membership order as
// CollectMemberTitles -- shared by every call site that refreshes tab
// labels, so titles and icons never drift out of sync with each other.
std::vector<HICON> CollectMemberIcons(const polish::GroupState& group) {
    std::vector<HICON> icons;
    for (const polish::GroupMember& member : group.Members()) {
        if (member.kind != polish::GroupMemberKind::Window || member.window == nullptr) {
            continue;  // nested-group case -- v1 never populates this
        }
        icons.push_back(polish::GetWindowIconHandle(member.window));
    }
    return icons;
}

// Called from OnWinEvent's EVENT_OBJECT_NAMECHANGE case (see the
// forward declaration near the group globals for why): if hwnd is a
// group member, refreshes its group's chrome tab labels from every
// member's *current* title -- previously a tab's label was only ever
// the title captured at creation/edit time, so e.g. a browser tab
// changing page or an editor gaining an unsaved-changes marker never
// showed up until something unrelated happened to repaint the chrome.
void OnMemberTitleChanged(HWND hwnd) {
    polish::GroupState* group = g_groupManager.FindGroupContaining(hwnd);
    if (group == nullptr) {
        return;
    }
    auto chromeIt = g_groupChromeWindows.find(group->Id());
    if (chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    chromeIt->second->SetMemberTitles(CollectMemberTitles(*group));
    chromeIt->second->SetMemberIcons(CollectMemberIcons(*group));
}

// Hides the tab hover-preview thumbnail (no-op if it isn't showing)
// and refreshes just the tab strip -- not the whole chrome window (a
// prior version of this fix used a plain InvalidateRect(..., nullptr,
// ...), which also repaints the content-area band behind the active
// member; that visibly overwrote it until something else forced it to
// repaint itself again -- a real, confirmed regression: File Explorer's
// content going blank after moving the mouse off a tab, until clicking
// the tab again triggered ReflowGroupTo's own explicit redraw). The
// thumbnail is a WS_EX_TOPMOST popup sitting right below the tab strip
// -- hiding it doesn't reliably trigger the chrome to repaint whatever
// sliver of the tab-strip/content boundary it was covering (same class
// of stale-composited-surface issue RedrawWindow already had to fix for
// member tab switches, see GroupManager's PositionMember), which left
// the active tab's highlight looking stale until something unrelated
// repainted it.
void HideGroupTabThumbnail(polish::GroupId id) {
    g_hoveredThumbnailMember = nullptr;
    KillTimer(g_messageWindow, kThumbnailStabilizeTimerId);
    if (!g_groupTabThumbnail) {
        return;
    }
    g_groupTabThumbnail->Hide();
    auto chromeIt = g_groupChromeWindows.find(id);
    if (chromeIt != g_groupChromeWindows.end()) {
        chromeIt->second->InvalidateTabStrip();
    }
}

// Called from GroupChromeWindow's onTabHovered callback: shows (or
// hides, if index is nullopt) a live thumbnail preview of the hovered
// tab's member window, so the user can see which window it is before
// committing to switch to it.
void OnGroupTabHovered(polish::GroupId id, std::optional<size_t> index, const RECT& tabScreenRect) {
    if (!index.has_value()) {
        HideGroupTabThumbnail(id);
        return;
    }
    polish::GroupState* group = g_groupManager.FindGroup(id);
    if (group == nullptr || *index >= group->Members().size()) {
        return;
    }
    // The active tab's content is already fully visible in the group --
    // nothing useful to preview, so don't show a thumbnail for it.
    if (group->ActiveIndex().has_value() && *group->ActiveIndex() == *index) {
        HideGroupTabThumbnail(id);
        return;
    }
    const polish::GroupMember& member = group->Members()[*index];
    if (member.kind != polish::GroupMemberKind::Window || member.window == nullptr || !IsWindow(member.window)) {
        return;
    }
    if (!g_groupTabThumbnail) {
        g_groupTabThumbnail = std::make_unique<polish::GroupTabThumbnail>(GetModuleHandleW(nullptr));
    }
    // A fresh, synchronous re-capture right now -- not just whatever
    // GroupManager's background sweep last cached -- so hovering
    // immediately after a group is created (before that sweep has even
    // fired) shows the best available snapshot right away, not a
    // possibly-stale one. See GroupTabThumbnail.h's comment for why a
    // *live* capture of an already-hidden member doesn't reliably work
    // on its own -- CaptureThumbnail's own message-flush/redraw
    // handling is still what makes this call meaningful.
    const bool preferRight = group->Alignment() == polish::GroupAlignment::Vertical;
    g_groupManager.RefreshThumbnail(member.window);
    g_groupTabThumbnail->ShowFor(g_groupManager.CachedThumbnail(member.window), tabScreenRect, preferRight);

    // Keep silently improving the shown preview for a bit in case its
    // content was still mid-load -- see kThumbnailStabilizeTimerId's
    // own comment for why (no universal "finished loading" signal
    // exists to just check once instead).
    g_hoveredThumbnailMember = member.window;
    g_hoveredThumbnailTabRect = tabScreenRect;
    g_hoveredThumbnailPreferRight = preferRight;
    g_thumbnailStabilizeAttemptsLeft = kThumbnailStabilizeMaxAttempts;
    SetTimer(g_messageWindow, kThumbnailStabilizeTimerId, kThumbnailStabilizeIntervalMs, nullptr);
}

// Fired by kThumbnailStabilizeTimerId while a tab's thumbnail is
// actively being hovered: re-captures it once more and, if the content
// actually changed, updates the already-shown popup with the newer
// image. Stops (kills its own timer) once a capture comes back
// unchanged -- content has settled -- or the attempt budget runs out;
// also implicitly stopped by HideGroupTabThumbnail clearing
// g_hoveredThumbnailMember whenever hover moves elsewhere first.
void StabilizeHoveredThumbnail() {
    if (g_hoveredThumbnailMember == nullptr || g_thumbnailStabilizeAttemptsLeft <= 0) {
        KillTimer(g_messageWindow, kThumbnailStabilizeTimerId);
        return;
    }
    --g_thumbnailStabilizeAttemptsLeft;
    const bool changed = g_groupManager.RefreshThumbnail(g_hoveredThumbnailMember);
    if (changed && g_groupTabThumbnail) {
        g_groupTabThumbnail->ShowFor(g_groupManager.CachedThumbnail(g_hoveredThumbnailMember),
                                      g_hoveredThumbnailTabRect, g_hoveredThumbnailPreferRight);
    }
    if (!changed || g_thumbnailStabilizeAttemptsLeft <= 0) {
        KillTimer(g_messageWindow, kThumbnailStabilizeTimerId);
        g_hoveredThumbnailMember = nullptr;
    }
}

// Called when a group's tab strip is clicked (GroupChromeWindow's
// onTabClicked callback): switches which member is active, both in the
// pure GroupState and the chrome's own visual highlight, reflows (so
// the newly active member is shown and every other one hidden), and
// actually focuses it. Members are real children now, so "focus" means
// SetFocus on the member after making sure the chrome itself is
// foreground -- not SetForegroundWindow on the member directly, which
// doesn't apply to a child window the way it does to a top-level one.
void ActivateGroupTab(polish::GroupId id, size_t index) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    // Clicking a tab commits to switching -- the hover preview (if
    // still showing, e.g. the click landed before the mouse settled
    // elsewhere) has nothing left to preview.
    HideGroupTabThumbnail(id);
    group->SetActiveIndex(index);
    chromeIt->second->SetActiveIndex(index);
    ReflowGroupTo(id);

    if (const auto active = group->ActiveWindow(); active.has_value() && IsWindow(*active)) {
        InjectHarmlessCtrlKeystroke();
        SetForegroundWindow(chromeIt->second->Handle());
        SetFocus(*active);
    }
    polish::LogDebug(std::format(L"[Polish] Group: tab {} activated for group id={}", index, id));
}

// Called from GroupChromeWindow's onTabReordered callback, live during
// a drag (once per tab crossed, not just once on drop -- see that
// callback's own comment) -- reorders GroupState's membership to match
// and rebuilds the chrome's tab labels from the new order, so the
// chrome's own array of labels never has to be reordered independently
// and risk drifting out of sync with GroupState.
void ReorderGroupTab(polish::GroupId id, size_t fromIndex, size_t toIndex) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    group->Reorder(fromIndex, toIndex);
    chromeIt->second->SetMemberTitles(CollectMemberTitles(*group));
    chromeIt->second->SetMemberIcons(CollectMemberIcons(*group));
    if (const auto activeIndex = group->ActiveIndex(); activeIndex.has_value()) {
        chromeIt->second->SetActiveIndex(*activeIndex);
    }
    polish::LogDebug(
        std::format(L"[Polish] Group: tab reordered {} -> {} for group id={}", fromIndex, toIndex, id));
}

// Names a mode for the debug log below.
const wchar_t* ModeName(polish::GroupMode mode) {
    switch (mode) {
        case polish::GroupMode::Tab:
            return L"Tab";
        case polish::GroupMode::Tile:
            return L"Tile";
        case polish::GroupMode::Stack:
            return L"Stack";
    }
    return L"Tab";
}

// Applies a new mode to group `id`: updates GroupState, mirrors it into
// the chrome's own rendering, and reflows -- ApplyLayout already
// branches on GroupState::Mode(), so switching modes needs no special-
// casing beyond that single flag flip plus a reflow. v1 has no per-mode
// saved geometry (a tiled member just gets repositioned into the shared
// tab rect if switching to Tab, and vice versa) -- acceptable for v1,
// not worth the complexity of remembering "where it would have been."
// Shared by ToggleGroupMode (title-bar button, cycles) and
// GroupChromeWindow's context-menu mode selection (picks directly).
void SetGroupMode(polish::GroupId id, polish::GroupMode mode) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    group->SetMode(mode);
    chromeIt->second->SetMode(mode);
    ReflowGroupTo(id);
    polish::LogDebug(std::format(L"[Polish] Group: mode switched to {} for group id={}", ModeName(mode), id));
}

// Called from GroupChromeWindow's title-bar mode-toggle button: cycles
// Tab -> Tile -> Stack -> Tab (see the chrome's own ModeToggle tooltip,
// which names whichever of these three is next).
void ToggleGroupMode(polish::GroupId id) {
    const polish::GroupState* group = g_groupManager.FindGroup(id);
    if (group == nullptr) {
        return;
    }
    polish::GroupMode newMode = polish::GroupMode::Tab;
    switch (group->Mode()) {
        case polish::GroupMode::Tab:
            newMode = polish::GroupMode::Tile;
            break;
        case polish::GroupMode::Tile:
            newMode = polish::GroupMode::Stack;
            break;
        case polish::GroupMode::Stack:
            newMode = polish::GroupMode::Tab;
            break;
    }
    SetGroupMode(id, newMode);
}

// Called from GroupChromeWindow's title-bar tile-maximize button (only
// ever clickable in a tiled mode -- Tile or Stack -- with 2+ members --
// the button itself isn't shown otherwise, see
// TileMaximizeButtonVisible). Same flip-state-then-reflow shape as
// ToggleGroupMode.
void ToggleTileMaximize(polish::GroupId id) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    const bool newMaximized = !group->IsTileMaximized();
    group->SetTileMaximized(newMaximized);
    chromeIt->second->SetTileMaximized(newMaximized);
    ReflowGroupTo(id);
    polish::LogDebug(std::format(L"[Polish] Group: tile {} for group id={}",
                                  newMaximized ? L"maximized" : L"restored", id));
}

// Called from GroupChromeWindow's title-bar alignment button. Same
// flip-state-then-reflow shape as ToggleGroupMode/ToggleTileMaximize --
// ReflowGroupTo re-derives everything from GroupState::Alignment()
// itself (GroupManager::ApplyTileLayout's grid-shape bias,
// GroupChromeWindow's own tab-strip axis via SetAlignment below), so
// flipping the one flag and reflowing is the whole job.
void ToggleGroupAlignment(polish::GroupId id) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    const polish::GroupAlignment newAlignment = (group->Alignment() == polish::GroupAlignment::Horizontal)
                                                     ? polish::GroupAlignment::Vertical
                                                     : polish::GroupAlignment::Horizontal;
    group->SetAlignment(newAlignment);
    chromeIt->second->SetAlignment(newAlignment);
    ReflowGroupTo(id);
    polish::LogDebug(std::format(L"[Polish] Group: alignment switched to {} for group id={}",
                                  newAlignment == polish::GroupAlignment::Vertical ? L"Vertical" : L"Horizontal",
                                  id));
}

// Called from GroupChromeWindow's "Edit windows..." context-menu item:
// reopens the picker pre-checked with the group's current membership
// and mode, then diffs the confirmed selection against current
// membership -- newly unchecked windows are removed (and released back
// to top-level -- GroupState::Remove alone only updates bookkeeping,
// it doesn't undo the reparenting), newly checked ones added
// (GroupState::AddWindow already handles active-index bookkeeping and
// duplicate/no-op safety; reparenting a newly-added member happens
// automatically the next time ApplyLayout runs, via ReflowGroupTo
// below).
void EditGroupWindows(polish::GroupId id) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }

    std::vector<HWND> currentMembers;
    for (const polish::GroupMember& member : group->Members()) {
        if (member.kind == polish::GroupMemberKind::Window && member.window != nullptr) {
            currentMembers.push_back(member.window);
        }
    }

    polish::GroupPickerWindow picker(GetModuleHandleW(nullptr));
    const auto result = picker.ShowModal(chromeIt->second->Handle(), currentMembers, group->Name(), true);
    if (!result.has_value()) {
        polish::LogDebug(L"[Polish] Group: edit-windows picker cancelled");
        return;
    }

    // Anything dropped from the confirmed Group list must be released
    // back to top-level (GroupState::SetMembers alone only updates
    // bookkeeping, it doesn't undo the reparenting) -- computed against
    // the old membership before SetMembers replaces it wholesale.
    for (HWND hwnd : currentMembers) {
        if (std::find(result->windows.begin(), result->windows.end(), hwnd) == result->windows.end()) {
            g_groupManager.ReleaseMember(hwnd);
        }
    }
    group->SetMembers(result->windows);
    group->SetName(result->name);

    chromeIt->second->SetMemberTitles(CollectMemberTitles(*group));
    chromeIt->second->SetMemberIcons(CollectMemberIcons(*group));
    if (const auto activeIndex = group->ActiveIndex(); activeIndex.has_value()) {
        chromeIt->second->SetActiveIndex(*activeIndex);
    }
    // SetMembers above may have just reset IsTileMaximized() to false
    // (membership dropped to <=1 -- see its own comment) -- re-sync the
    // chrome's copy either way so the tile-maximize button's glyph/
    // visibility reflects whatever GroupState actually landed on, not
    // whatever it was before this edit.
    chromeIt->second->SetTileMaximized(group->IsTileMaximized());
    SetWindowTextW(chromeIt->second->Handle(), group->Name().c_str());
    ReflowGroupTo(id);
    polish::LogDebug(std::format(L"[Polish] Group: edited group id={}, now {} window(s), name=\"{}\"", id,
                                  group->MemberCount(), group->Name()));
}

// Message id for the deferred close-group cleanup below (WM_APP+1 and
// WM_APP+10 are already taken by TrayIcon/AltTabHook).
constexpr UINT kCloseGroupMessage = WM_APP + 20;

// Called from GroupChromeWindow's onClosing callback (WM_CLOSE, before
// the window is actually destroyed): releases every member back to an
// independent top-level window so none of them are destroyed along with
// the chrome (see WindowReparenting.h). Does *not* touch
// g_groupChromeWindows here -- this runs from inside the very chrome
// window's own WM_CLOSE handling, so erasing its map entry now would
// delete the GroupChromeWindow object (running its DestroyWindow-calling
// destructor) while still unwinding that object's own call stack.
// Posting kCloseGroupMessage instead defers the actual erase to a clean,
// top-level point in the message loop, once WM_CLOSE/WM_DESTROY have
// both already fully finished.
void CloseGroup(polish::GroupId id) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    if (group == nullptr) {
        return;
    }
    g_groupManager.ReleaseGroup(*group);
    polish::LogDebug(std::format(L"[Polish] Group: closed group id={}, {} member(s) released to top-level", id,
                                  group->MemberCount()));
    PostMessageW(g_messageWindow, kCloseGroupMessage, static_cast<WPARAM>(id), 0);
}

// Triggered by both the tray menu's "New Group" item and the Win+Alt+G
// hotkey: shows the window picker, and on confirm hands the selection
// straight to GroupManager. `owner` centers the picker and is passed
// through as its Win32 owner window -- may be nullptr (e.g. triggered
// via the hotkey with no natural owner), which GroupPickerWindow
// already falls back on (primary monitor).
void TriggerNewGroup(HWND owner) {
    polish::GroupPickerWindow picker(GetModuleHandleW(nullptr));
    const auto selection = picker.ShowModal(owner, {}, L"New Group", false);
    if (!selection.has_value()) {
        polish::LogDebug(L"[Polish] New Group: picker cancelled");
        return;
    }
    // Mode is no longer chosen in the picker (moving to the chrome's
    // own title-bar controls) -- every new group starts in Tab mode,
    // matching GroupState's own default; the existing context-menu
    // toggle can still switch it afterward.
    const polish::GroupId id = g_groupManager.CreateGroup(selection->windows);
    polish::GroupState* newGroup = g_groupManager.FindGroup(id);
    if (newGroup != nullptr) {
        newGroup->SetName(selection->name);
    }
    polish::LogDebug(std::format(L"[Polish] New Group: created group id={} with {} window(s), name=\"{}\"", id,
                                  selection->windows.size(), selection->name));
    std::vector<std::wstring> memberTitles;
    for (HWND hwnd : selection->windows) {
        wchar_t title[256] = L"";
        GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        polish::LogDebug(
            std::format(L"[Polish] New Group: member hwnd={} title=\"{}\"", reinterpret_cast<void*>(hwnd), title));
        memberTitles.emplace_back(title);
    }

    std::vector<HICON> memberIcons;
    for (HWND hwnd : selection->windows) {
        memberIcons.push_back(polish::GetWindowIconHandle(hwnd));
    }

    auto chrome = std::make_unique<polish::GroupChromeWindow>(GetModuleHandleW(nullptr));
    chrome->Show(memberTitles);
    SetWindowTextW(chrome->Handle(), selection->name.c_str());
    chrome->SetMemberIcons(memberIcons);
    chrome->SetOnTabClicked([id](size_t index) { ActivateGroupTab(id, index); });
    chrome->SetOnTabReordered([id](size_t from, size_t to) { ReorderGroupTab(id, from, to); });
    chrome->SetOnModeToggleRequested([id]() { ToggleGroupMode(id); });
    chrome->SetOnModeSelected([id](polish::GroupMode mode) { SetGroupMode(id, mode); });
    chrome->SetOnTileMaximizeToggleRequested([id]() { ToggleTileMaximize(id); });
    chrome->SetOnAlignmentToggleRequested([id]() { ToggleGroupAlignment(id); });
    chrome->SetOnEditWindowsRequested([id]() { EditGroupWindows(id); });
    chrome->SetOnResized([id]() { ReflowGroupTo(id); });
    // Deliberately UpdateGroupActiveTileHighlight, not a full
    // ReflowGroupTo -- a member moves for free with its parent (see
    // GroupChromeWindow's own WM_MOVE comment), only the ring needs
    // repositioning.
    chrome->SetOnMoved([id]() { UpdateGroupActiveTileHighlight(id); });
    chrome->SetOnMemberClicked([id](POINT pt) { OnGroupMemberClicked(id, pt); });
    chrome->SetOnClosing([id]() { CloseGroup(id); });
    chrome->SetOnTabHovered(
        [id](std::optional<size_t> index, const RECT& tabScreenRect) { OnGroupTabHovered(id, index, tabScreenRect); });
    chrome->SetOnTileSplitterDragged(
        [id](bool column, size_t index, int newPixelPosition) {
            OnTileSplitterDragged(id, column, index, newPixelPosition);
        });
    chrome->SetOnTileSplitterDoubleClicked(
        [id](bool column, size_t index) { OnTileSplitterDoubleClicked(id, column, index); });
    polish::LogDebug(std::format(L"[Polish] New Group: chrome window created hwnd={} for group id={}",
                                  reinterpret_cast<void*>(chrome->Handle()), id));

    g_groupChromeWindows[id] = std::move(chrome);
    ReflowGroupTo(id);
}

// Rebuilt fresh every time the tray icon's context menu is about to
// open (see TrayIcon's populateMenu callback), so checkbox state is
// always current -- in particular Start with Windows, whose real source
// of truth is the Run key itself, not a cached value here.
void PopulateTrayMenu(HMENU menu) {
    AppendMenuW(menu, MF_STRING | (g_settings.restoreSyncEnabled ? MF_CHECKED : MF_UNCHECKED), kMenuIdRestoreSync,
                L"Restore remembers Snap position");
    AppendMenuW(menu, MF_STRING | (g_settings.altTabEnabled ? MF_CHECKED : MF_UNCHECKED), kMenuIdAltTab,
                L"Alt+Tab and Alt+` switchers");
    AppendMenuW(menu, MF_STRING | (g_settings.haloEnabled ? MF_CHECKED : MF_UNCHECKED), kMenuIdHalo,
                L"Halo around active window");
    AppendMenuW(menu, MF_STRING | (g_settings.bullseyeEnabled ? MF_CHECKED : MF_UNCHECKED), kMenuIdBullseye,
                L"Bullseye (copy/paste flash)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuIdNewGroup,
                (L"New Group...\t" + FormatHotkey(g_settings.groupHotkeyModifiers, g_settings.groupHotkeyVirtualKey))
                    .c_str());
    AppendMenuW(menu, MF_STRING, kMenuIdChangeGroupHotkey, L"Change Group Hotkey...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (polish::IsStartAtLoginEnabled() ? MF_CHECKED : MF_UNCHECKED),
                kMenuIdStartAtLogin, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuIdAbout, L"By David Lenihan. Hire me!");
    AppendMenuW(menu, MF_STRING, kMenuIdExit, L"Exit");
}

void HandleTrayCommand(UINT commandId) {
    switch (commandId) {
        case kMenuIdRestoreSync:
            g_settings.restoreSyncEnabled = !g_settings.restoreSyncEnabled;
            polish::SaveSettings(g_settings);
            polish::LogDebug(std::format(L"[Polish] restore-position sync {}",
                                          g_settings.restoreSyncEnabled ? L"enabled" : L"disabled"));
            break;
        case kMenuIdAltTab:
            g_settings.altTabEnabled = !g_settings.altTabEnabled;
            polish::SaveSettings(g_settings);
            polish::LogDebug(
                std::format(L"[Polish] Alt+Tab {}", g_settings.altTabEnabled ? L"enabled" : L"disabled"));
            break;
        case kMenuIdHalo:
            g_settings.haloEnabled = !g_settings.haloEnabled;
            polish::SaveSettings(g_settings);
            polish::LogDebug(std::format(L"[Polish] Halo {}", g_settings.haloEnabled ? L"enabled" : L"disabled"));
            UpdateActiveWindowHalo(GetForegroundWindow());
            break;
        case kMenuIdBullseye:
            g_settings.bullseyeEnabled = !g_settings.bullseyeEnabled;
            polish::SaveSettings(g_settings);
            polish::LogDebug(
                std::format(L"[Polish] Bullseye {}", g_settings.bullseyeEnabled ? L"enabled" : L"disabled"));
            if (!g_settings.bullseyeEnabled && g_bullseye) {
                // Mid-animation toggle-off should disappear now, not
                // after the ring finishes; the frame timer's next tick
                // sees an inactive overlay and kills itself.
                g_bullseye->Hide();
            }
            break;
        case kMenuIdStartAtLogin: {
            const bool newValue = !polish::IsStartAtLoginEnabled();
            polish::SetStartAtLoginEnabled(newValue);
            polish::LogDebug(
                std::format(L"[Polish] start with Windows {}", newValue ? L"enabled" : L"disabled"));
            break;
        }
        case kMenuIdNewGroup:
            TriggerNewGroup(nullptr);
            break;
        case kMenuIdChangeGroupHotkey:
            ChangeGroupHotkey();
            break;
        case kMenuIdAbout:
            ShellExecuteW(nullptr, L"open", kAboutUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kMenuIdExit:
            DestroyWindow(g_messageWindow);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
        if (g_trayIcon) {
            g_trayIcon->HandleTaskbarRecreated();
        }
        return 0;
    }

    switch (message) {
        case WM_TIMER:
            if (wParam == kSettleTimerId) {
                KillTimer(hwnd, kSettleTimerId);
                CheckSettledRectAndRecord();
            } else if (wParam == kThumbnailRefreshTimerId) {
                KillTimer(hwnd, kThumbnailRefreshTimerId);
                RefreshAllHiddenThumbnails();
            } else if (wParam == kThumbnailStabilizeTimerId) {
                StabilizeHoveredThumbnail();
            } else if (wParam == kHaloRenderTimerId) {
                KillTimer(hwnd, kHaloRenderTimerId);
                if (g_haloWatched != nullptr) {
                    UpdateActiveWindowHalo(g_haloWatched);
                }
            } else if (wParam == kHaloRestoreTimerId) {
                // The restore animation has had time to finish -- see
                // kHaloRestoreTimerId. Resolve the foreground window now
                // rather than remembering the one that was restoring: focus
                // may well have moved on during the wait.
                KillTimer(hwnd, kHaloRestoreTimerId);
                g_haloMinimizeSuppressed = false;
                UpdateActiveWindowHalo(GetForegroundWindow());
            } else if (wParam == kBullseyeAnchorTimerId) {
                ResolveBullseyeAnchor(/*timedOut=*/true);
            } else if (wParam == kBullseyeFrameTimerId) {
                if (!g_bullseye || !g_bullseye->AdvanceFrame()) {
                    KillTimer(hwnd, kBullseyeFrameTimerId);
                }
            }
            return 0;

        case WM_CLIPBOARDUPDATE:
            OnClipboardUpdated();
            return 0;

        case polish::TrayIcon::kCallbackMessage:
            if (g_trayIcon) {
                g_trayIcon->HandleCallbackMessage(lParam);
            }
            return 0;

        case polish::AltTabHook::kHookMessage:
            if (g_altTabHook) {
                g_altTabHook->HandleHookMessage(wParam, lParam);
            }
            return 0;

        case polish::kTabsReadyMessage:
            OnTabsReady(static_cast<uint64_t>(wParam));
            return 0;

        case polish::kSelectionReadyMessage:
            if (static_cast<uint64_t>(wParam) == g_pendingBullseyeGeneration) {
                ResolveBullseyeAnchor(/*timedOut=*/false);
            }
            return 0;

        case WM_COMMAND:
            if (g_trayIcon) {
                g_trayIcon->HandleCommand(wParam);
            }
            return 0;

        case WM_HOTKEY:
            if (wParam == kNewGroupHotkeyId) {
                TriggerNewGroup(nullptr);
            }
            return 0;

        case WM_DISPLAYCHANGE:
            // Monitor connected/disconnected, or a resolution/topology
            // change -- rebuild Alt+Tab's per-monitor panels against the
            // new reality. See RefreshAltTabPanels' own comment for the
            // real, human-reported bug this fixes.
            RefreshAltTabPanels();
            // Monitor topology changing can change which monitor the
            // halo's own target is on (and therefore its DPI and flush-
            // edge inflation) without any LOCATIONCHANGE for the target
            // itself.
            UpdateActiveWindowHalo(GetForegroundWindow());
            return 0;

        case WM_SETTINGCHANGE:
            // ImmersiveColorSet -- the user flipped Settings > Personalization
            // > Colors' light/dark toggle. This message window is a real,
            // if invisible, WS_OVERLAPPEDWINDOW top-level window (see
            // CreateMessageWindow), so it does receive the broadcast.
            // IsDarkModeEnabled() is read fresh on every halo render
            // already (never cached -- the codebase-wide convention), so
            // just asking for a re-render is enough to pick up the change
            // live.
            if (lParam != 0 && lstrcmpiW(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
                UpdateActiveWindowHalo(GetForegroundWindow());
            }
            return 0;

        case kCloseGroupMessage: {
            // Deferred from CloseGroup -- see its own comment for why
            // this can't safely happen synchronously from within the
            // closing chrome's own WM_CLOSE handling. By now WM_CLOSE/
            // WM_DESTROY have both fully finished and members have
            // already been released, so erasing (and thereby
            // destroying, via ~GroupChromeWindow) is safe here.
            const auto id = static_cast<polish::GroupId>(wParam);
            // Erase the ring *before* the chrome: the ring is owned by
            // the chrome window (see AltTabHighlightBorder's `owner`),
            // so destroying the chrome first would destroy the ring
            // along with it as a side effect (harmless -- the ring's own
            // destructor guards with IsWindow -- but erasing our side
            // first is the more predictable order).
            g_groupActiveTileHighlights.erase(id);
            g_groupChromeWindows.erase(id);
            return 0;
        }

        case WM_DESTROY:
            RemoveClipboardFormatListener(hwnd);
            KillTimer(hwnd, kBullseyeFrameTimerId);
            UnregisterHotKey(hwnd, kNewGroupHotkeyId);
            if (g_foregroundHook != nullptr) {
                UnhookWinEvent(g_foregroundHook);
            }
            if (g_locationChangeHook != nullptr) {
                UnhookWinEvent(g_locationChangeHook);
            }
            if (g_moveSizeStartHook != nullptr) {
                UnhookWinEvent(g_moveSizeStartHook);
            }
            if (g_moveSizeEndHook != nullptr) {
                UnhookWinEvent(g_moveSizeEndHook);
            }
            if (g_destroyHook != nullptr) {
                UnhookWinEvent(g_destroyHook);
            }
            if (g_nameChangeHook != nullptr) {
                UnhookWinEvent(g_nameChangeHook);
            }
            if (g_objectFocusHook != nullptr) {
                UnhookWinEvent(g_objectFocusHook);
            }
            if (g_minimizeStartHook != nullptr) {
                UnhookWinEvent(g_minimizeStartHook);
            }
            if (g_minimizeEndHook != nullptr) {
                UnhookWinEvent(g_minimizeEndHook);
            }
            g_trayIcon.reset();
            g_altTabHook.reset();
            g_altTabOverlays.clear();
            g_activeWindowHalo.reset();
            g_bullseye.reset();
            // Before g_groupChromeWindows.clear() below -- each ring is
            // owned by its group's chrome (see AltTabHighlightBorder's
            // `owner`), so clearing this first is the same predictable-
            // order reasoning as kCloseGroupMessage's own erase order.
            g_groupActiveTileHighlights.clear();
            // Every remaining group's members must be released back to
            // top-level *before* their chrome windows are destroyed
            // below -- unlike a single group's own WM_CLOSE path
            // (GroupChromeWindow::SetOnClosing/CloseGroup), destroying
            // this message window directly never sends WM_CLOSE to any
            // chrome at all, so nothing else does this for app exit.
            // Skipping it would destroy every still-grouped real window
            // (Outlook, VS Code, etc.) along with its chrome.
            for (const polish::GroupState& group : g_groupManager.Groups()) {
                g_groupManager.ReleaseGroup(group);
            }
            g_groupChromeWindows.clear();
            g_groupTabThumbnail.reset();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

// A hidden, ordinary top-level window (not HWND_MESSAGE-parented): tray
// icon menus need SetForegroundWindow/TrackPopupMenuEx to work, which is
// unreliable on a true message-only window. Never shown.
HWND CreateMessageWindow(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = MessageWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kMessageWindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        return nullptr;
    }

    return CreateWindowExW(0, kMessageWindowClassName, L"Polish", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, nullptr,
                            nullptr, instance, nullptr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (singleInstanceMutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstanceMutex);
        return 0;
    }

    polish::LogStartupBanner();
    LogDpiAwareness();

    // COINIT_APARTMENTTHREADED -- this app has exactly one thread that
    // ever touches a window or COM object, the classic STA shape.
    // Needed before anything can resolve a packaged app's real icon
    // (util/WindowIcon.cpp's GetPackagedAppIcon, via IShellItem/
    // IShellItemImageFactory -- CoCreateInstance and friends require COM
    // to already be initialized on the calling thread) -- done once,
    // here, rather than lazily at first use, so a failure is visible in
    // the log right at startup instead of buried inside whatever picker
    // row happens to need an icon first. A failure here isn't fatal to
    // the rest of the app -- it would just mean a packaged app's row
    // falls back to the generic executable icon, a cosmetic regression,
    // not a reason to refuse to start.
    const HRESULT comInitResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comInitResult)) {
        polish::LogDebug(std::format(L"[Polish] CoInitializeEx failed, hr=0x{:08x} -- packaged apps will fall back "
                                      L"to their generic executable icon",
                                      static_cast<unsigned long>(comInitResult)));
    }

    g_settings = polish::LoadSettings();
    polish::LogDebug(std::format(
        L"[Polish] settings loaded: restoreSyncEnabled={} altTabEnabled={} haloEnabled={} bullseyeEnabled={}",
        g_settings.restoreSyncEnabled, g_settings.altTabEnabled, g_settings.haloEnabled,
        g_settings.bullseyeEnabled));

    g_messageWindow = CreateMessageWindow(instance);
    if (g_messageWindow == nullptr) {
        CloseHandle(singleInstanceMutex);
        return 1;
    }

    if (!AddClipboardFormatListener(g_messageWindow)) {
        polish::LogDebug(std::format(L"[Polish] WARNING: failed to register the clipboard listener (bullseye "
                                      L"copy detection unavailable). GetLastError={}",
                                      GetLastError()));
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent,
                                        0, 0, WINEVENT_OUTOFCONTEXT);
    g_locationChangeHook =
        SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent, 0, 0,
                         WINEVENT_OUTOFCONTEXT);
    g_moveSizeStartHook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZESTART, nullptr,
                                           OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_moveSizeEndHook = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr,
                                         OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_destroyHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, OnWinEvent, 0, 0,
                                     WINEVENT_OUTOFCONTEXT);
    g_nameChangeHook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, OnWinEvent, 0, 0,
                                        WINEVENT_OUTOFCONTEXT);
    g_objectFocusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, OnWinEvent, 0, 0,
                                         WINEVENT_OUTOFCONTEXT);
    g_minimizeStartHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr,
                                           OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_minimizeEndHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0,
                                         0, WINEVENT_OUTOFCONTEXT);

    g_trayIcon = std::make_unique<polish::TrayIcon>(g_messageWindow, PopulateTrayMenu, HandleTrayCommand);

    if (!RegisterGroupHotkeyFromSettings()) {
        polish::LogDebug(std::format(
            L"[Polish] WARNING: failed to register the {} hotkey (already in use by something else on this "
            L"PC?). GetLastError={}. Use the tray menu's \"Change Group Hotkey...\" to pick a different one.",
            FormatHotkey(g_settings.groupHotkeyModifiers, g_settings.groupHotkeyVirtualKey), GetLastError()));
    } else {
        polish::LogDebug(std::format(
            L"[Polish] {} (New Group) hotkey registered successfully",
            FormatHotkey(g_settings.groupHotkeyModifiers, g_settings.groupHotkeyVirtualKey)));
    }

    // Started before the hook: AltTabEligibility consults it synchronously
    // on the hook thread, and a null worker there just means Alt+` reports
    // nothing to switch to.
    g_uiaWorker = std::make_unique<polish::UiaWorker>(g_messageWindow);

    g_altTabHook = std::make_unique<polish::AltTabHook>(g_messageWindow, AltTabEligibility,
                                                         OnAltTabCycle, OnAltTabCommit, OnAltTabCancel);
    if (!g_altTabHook->IsInstalled()) {
        polish::LogDebug(std::format(L"[Polish] WARNING: failed to install the Alt+Tab keyboard hook. "
                                      L"GetLastError={}",
                                      GetLastError()));
    } else {
        polish::LogDebug(L"[Polish] Alt+Tab keyboard hook installed successfully");
    }
    // Alt+` is bound to VK_OEM_3, which is the backtick key only on US/UK
    // layouts (it is a letter on German, "<" on others). Logged so a
    // "Alt+` does nothing on my machine" report is diagnosable from the log.
    {
        const UINT printed = MapVirtualKeyW(VK_OEM_3, MAPVK_VK_TO_CHAR) & 0x7FFFFFFF;  // high bit = dead key
        polish::LogDebug(std::format(L"[Polish] Alt+` is bound to VK_OEM_3, which types '{}' on the active keyboard layout",
                                      printed != 0 ? std::wstring(1, static_cast<wchar_t>(printed)) : std::wstring(L"?")));
    }
    g_altTabHook->SetOnNavigate(OnAltTabNavigate);
    g_altTabHook->SetOnPasteChord([] { PlayBullseye(polish::BullseyePhase::Paste); });
    // Del/-/+ act on whichever row Tab-cycling currently has highlighted
    // -- there's no keyboard equivalent of a mouse hover, so this is
    // always CurrentAltTabHighlightedWindow(), unlike the panel's own
    // mouse-driven callbacks (SetOnRowClose etc.), which can also fire
    // for a merely-hovered, non-highlighted row.
    g_altTabHook->SetOnRowAction([](polish::AltTabHook::RowAction action) {
        const HWND hwnd = CurrentAltTabHighlightedWindow();
        switch (action) {
            case polish::AltTabHook::RowAction::Close:
                OnAltTabRowClose(hwnd);
                break;
            case polish::AltTabHook::RowAction::MinimizeToggle:
                OnAltTabRowMinimizeToggle(hwnd);
                break;
            case polish::AltTabHook::RowAction::MaximizeToggle:
                OnAltTabRowMaximizeToggle(hwnd);
                break;
        }
    });
    // Lets AltTabHook's mouse hook tell "a click on one of the list
    // panels" apart from "a click anywhere else" (which commits the
    // session) -- see AltTabHook's class comment on this second,
    // deliberately-bounded hook-thread exception.
    g_altTabHook->SetIsOwnUI([](POINT screenPt) {
        return std::any_of(g_altTabPanels.begin(), g_altTabPanels.end(),
                            [screenPt](const AltTabMonitorPanel& panel) { return panel.window->ContainsPoint(screenPt); });
    });

    // Pre-create the dim overlays, highlight border, and one list panel
    // per connected monitor now, at startup, rather than paying
    // CreateWindowExW + first-paint latency in response to the user's
    // actual first Tab press -- confirmed as a real, visible glitch (the
    // previously-active window briefly still looked highlighted/undimmed
    // before the real highlight caught up). EnsureAltTabOverlayPoolSize
    // only ever grows the pool, so this is purely a head start for the
    // common case, not a hard requirement -- it still grows safely later
    // if more windows open than were open right now.
    RebuildAltTabCandidates();
    EnsureAltTabOverlayPoolSize(g_altTabDimTargets.size());
    RefreshAltTabPanels();

    // Same pre-creation reasoning as the Alt+Tab overlays above -- pay
    // CreateWindowExW's cost now rather than on the first real foreground
    // change.
    g_activeWindowHalo = std::make_unique<polish::ActiveWindowHalo>(instance);
    g_bullseye = std::make_unique<polish::BullseyeOverlay>(instance);

    OnForegroundChanged(GetForegroundWindow());

    MSG msg;
    BOOL getMessageResult;
    while ((getMessageResult = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (getMessageResult == -1) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(singleInstanceMutex);
    // Only if CoInitializeEx actually succeeded above -- CoUninitialize
    // without a matching successful init call is undefined behavior, not
    // a no-op.
    if (SUCCEEDED(comInitResult)) {
        CoUninitialize();
    }
    return static_cast<int>(msg.wParam);
}

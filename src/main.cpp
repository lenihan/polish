#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "hook/AltTabDimOverlay.h"
#include "hook/AltTabHighlightBorder.h"
#include "hook/AltTabHook.h"
#include "hook/GroupChromeWindow.h"
#include "hook/GroupHotkeyDialog.h"
#include "hook/GroupPickerWindow.h"
#include "hook/GroupTabThumbnail.h"
#include "settings/Settings.h"
#include "tray/TrayIcon.h"
#include "util/Logging.h"
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

HWND g_messageWindow = nullptr;
HWINEVENTHOOK g_foregroundHook = nullptr;
HWINEVENTHOOK g_locationChangeHook = nullptr;
HWINEVENTHOOK g_moveSizeStartHook = nullptr;
HWINEVENTHOOK g_moveSizeEndHook = nullptr;
HWINEVENTHOOK g_destroyHook = nullptr;
HWINEVENTHOOK g_nameChangeHook = nullptr;
UINT g_taskbarCreatedMessage = 0;

std::unique_ptr<polish::TrayIcon> g_trayIcon;

// User-toggleable feature flags, loaded once at startup and updated (+
// persisted) whenever the tray menu checkboxes are toggled -- see
// PopulateTrayMenu/HandleTrayCommand.
polish::Settings g_settings;
std::unique_ptr<polish::AltTabHook> g_altTabHook;

// Alt+Tab switcher session state. A snapshot of candidates is taken once
// when a session starts (first Tab press) and used for the whole
// session -- if a candidate closes mid-session its overlay just sits
// over whatever's left there until the session ends; not handled more
// gracefully than that yet. The overlay pool only ever grows (indices
// beyond the current session's candidate count just stay hidden), so
// repeated sessions don't pay window-creation cost more than once per
// "most windows ever open at once this run."
bool g_altTabSessionOpen = false;
std::vector<HWND> g_altTabCandidates;
size_t g_altTabHighlightIndex = 0;
std::vector<std::unique_ptr<polish::AltTabDimOverlay>> g_altTabOverlays;
// Only one window is ever highlighted at a time, unlike the dim overlays
// (one per non-highlighted candidate), so this is a single instance, not
// a pool.
std::unique_ptr<polish::AltTabHighlightBorder> g_altTabHighlightBorder;

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
int g_thumbnailStabilizeAttemptsLeft = 0;

// Forward-declared: defined near the rest of the group-management code
// (TriggerNewGroup etc.), further down; called from OnWinEvent's
// EVENT_OBJECT_NAMECHANGE case, above that point. No-op unless hwnd is
// a group member -- a member's title changing is common (browser tabs,
// unsaved-changes markers, etc.) and previously just sat stale in its
// group's tab label until something else (a tab click, a
// drag) happened to trigger a repaint.
void OnMemberTitleChanged(HWND hwnd);

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

void OnForegroundChanged(HWND newForeground) {
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
            break;

        case EVENT_OBJECT_LOCATIONCHANGE:
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
            break;

        case EVENT_SYSTEM_MOVESIZEEND:
            if (hwnd == g_trackedWindow) {
                g_inMoveSizeLoop = false;
                KillTimer(g_messageWindow, kSettleTimerId);
                CheckSettledRectAndRecord();
            }
            break;

        case EVENT_OBJECT_DESTROY:
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
                g_activationHistory.Remove(hwnd);
                if (hwnd == g_trackedWindow) {
                    g_trackedWindow = nullptr;
                    g_inMoveSizeLoop = false;
                    g_pendingSettleRect.reset();
                    KillTimer(g_messageWindow, kSettleTimerId);
                }
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

        default:
            break;
    }
}

BOOL CALLBACK EnumCandidateWindowsProc(HWND hwnd, LPARAM lParam) {
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
void RebuildAltTabCandidates() {
    std::vector<HWND> allCandidates;
    EnumWindows(EnumCandidateWindowsProc, reinterpret_cast<LPARAM>(&allCandidates));

    g_altTabCandidates.clear();
    for (HWND hwnd : g_activationHistory.OrderedWindows()) {
        if (std::find(allCandidates.begin(), allCandidates.end(), hwnd) != allCandidates.end()) {
            g_altTabCandidates.push_back(hwnd);
        }
    }
    for (HWND hwnd : allCandidates) {
        if (std::find(g_altTabCandidates.begin(), g_altTabCandidates.end(), hwnd) ==
            g_altTabCandidates.end()) {
            g_altTabCandidates.push_back(hwnd);
        }
    }
}

// Called synchronously from inside AltTabHook's low-level hook callback
// (see its class comment for why that's safe here) to decide whether
// Alt+Tab should be intercepted at all. Rebuilds the candidate list as a
// side effect -- by the time a real session starts, g_altTabCandidates
// is already correct and OnAltTabCycle doesn't need to rebuild it again.
bool AltTabHasEligibleCandidates() {
    if (!g_settings.altTabEnabled) {
        return false;  // native Alt+Tab runs untouched -- see AltTabHook
    }
    RebuildAltTabCandidates();
    return g_altTabCandidates.size() >= 2;
}

void EnsureAltTabOverlayPoolSize(size_t count) {
    while (g_altTabOverlays.size() < count) {
        g_altTabOverlays.push_back(std::make_unique<polish::AltTabDimOverlay>(GetModuleHandleW(nullptr)));
    }
}

void EnsureAltTabHighlightBorder() {
    if (!g_altTabHighlightBorder) {
        g_altTabHighlightBorder = std::make_unique<polish::AltTabHighlightBorder>(GetModuleHandleW(nullptr));
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
void ApplyAltTabDimming() {
    const ULONGLONG t0 = GetTickCount64();
    for (size_t i = 0; i < g_altTabCandidates.size(); ++i) {
        if (i == g_altTabHighlightIndex) {
            continue;  // handled last, below
        }
        HWND hwnd = g_altTabCandidates[i];
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_altTabOverlays[i]->ShowOverTarget(hwnd);
    }
    const ULONGLONG t1 = GetTickCount64();

    const HWND highlighted = g_altTabCandidates[g_altTabHighlightIndex];
    g_altTabOverlays[g_altTabHighlightIndex]->Hide();
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
    const ULONGLONG t2 = GetTickCount64();

    EnsureAltTabHighlightBorder();
    g_altTabHighlightBorder->ShowAroundTarget(highlighted);
    const ULONGLONG t3 = GetTickCount64();

    // Timing breadcrumbs to chase a human-reported "flash of the
    // previous window" glitch with real evidence -- a pre-warming fix
    // (creating the overlay/border windows at startup instead of lazily)
    // didn't resolve it, so the cause is still unconfirmed.
    polish::LogDebug(std::format(
        L"[Polish] AltTab: dimming timing -- otherOverlays={}ms highlightPromote={}ms highlightBorder={}ms",
        t1 - t0, t2 - t1, t3 - t2));
}

void EndAltTabSession() {
    for (auto& overlay : g_altTabOverlays) {
        overlay->Hide();
    }
    if (g_altTabHighlightBorder) {
        g_altTabHighlightBorder->Hide();
    }
    g_altTabSessionOpen = false;
}

void OnAltTabCycle(bool backward) {
    if (g_altTabHook) {
        const ULONGLONG queueDelayMs = GetTickCount64() - g_altTabHook->LastTabDetectedTick();
        polish::LogDebug(
            std::format(L"[Polish] AltTab: message-queue delay since Tab detected: {}ms", queueDelayMs));
    }
    if (g_altTabCandidates.empty()) {
        // Defensive only -- shouldn't happen. AltTabHook's
        // hasEligibleCandidates callback (AltTabHasEligibleCandidates,
        // which rebuilds g_altTabCandidates as a side effect) already
        // guarantees at least 2 entries before this ever fires for a new
        // session, and both run on the same thread with nothing else
        // able to run in between.
        return;
    }
    if (!g_altTabSessionOpen) {
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
        polish::LogDebug(std::format(L"[Polish] AltTab: session starting, {} candidate(s): {}",
                                      g_altTabCandidates.size(), candidateDump));
        EnsureAltTabOverlayPoolSize(g_altTabCandidates.size());
        // Index 0 is the current window itself (freshest in the MRU
        // order); the first Tab press should land on the previous
        // window, matching native Alt+Tab's single-tap-swap behavior.
        g_altTabHighlightIndex = 1 % g_altTabCandidates.size();
        g_altTabSessionOpen = true;
    } else {
        const size_t count = g_altTabCandidates.size();
        g_altTabHighlightIndex =
            backward ? (g_altTabHighlightIndex + count - 1) % count : (g_altTabHighlightIndex + 1) % count;
    }
    ApplyAltTabDimming();
    polish::LogDebug(std::format(L"[Polish] AltTab: cycle {} -> highlighting hwnd={}",
                                  backward ? L"backward" : L"forward",
                                  reinterpret_cast<void*>(g_altTabCandidates[g_altTabHighlightIndex])));
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
    if (!g_altTabSessionOpen) {
        return;
    }
    const HWND target = g_altTabCandidates[g_altTabHighlightIndex];
    EndAltTabSession();
    if (IsWindow(target)) {
        InjectHarmlessCtrlKeystroke();
        const BOOL result = SetForegroundWindow(target);
        polish::LogDebug(std::format(
            L"[Polish] AltTab: commit -> hwnd={} SetForegroundWindow result={} actualForeground={}",
            reinterpret_cast<void*>(target), result != FALSE, reinterpret_cast<void*>(GetForegroundWindow())));
        return;
    }
    polish::LogDebug(std::format(L"[Polish] AltTab: commit -> hwnd={} no longer a valid window",
                                  reinterpret_cast<void*>(target)));
}

void OnAltTabCancel() {
    if (!g_altTabSessionOpen) {
        return;
    }
    EndAltTabSession();
    polish::LogDebug(L"[Polish] AltTab: cancel");
}

constexpr UINT kMenuIdRestoreSync = 1;
constexpr UINT kMenuIdAltTab = 2;
constexpr UINT kMenuIdNewGroup = 3;
constexpr UINT kMenuIdChangeGroupHotkey = 4;
constexpr UINT kMenuIdStartAtLogin = 5;
constexpr UINT kMenuIdAbout = 6;
constexpr UINT kMenuIdExit = 7;

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
    polish::LogDebug(std::format(L"[Polish][DIAG] ReflowGroupTo id={} reentrant={}", id, g_reflowGrowInProgress));
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
    const SIZE needed =
        g_groupManager.ApplyLayout(*group, chromeHandle, contentRect, chromeIt->second->TileSplitterWidthPx());
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
    if (needed.cx <= requestedWidth && needed.cy <= requestedHeight) {
        return;
    }

    polish::LogDebug(std::format(
        L"[Polish] Group: member(s) wouldn't fit group id={} (requested {}x{}, needed {}x{}) -- growing chrome",
        id, requestedWidth, requestedHeight, needed.cx, needed.cy));
    g_reflowGrowInProgress = true;
    chromeIt->second->GrowContentAreaTo(needed);
    g_reflowGrowInProgress = false;
    g_groupManager.ApplyLayout(*group, chromeHandle, chromeIt->second->ContentRectInClientCoords(),
                                chromeIt->second->TileSplitterWidthPx());
    chromeIt->second->SetTileSplitters(g_groupManager.TileColumnBoundaries(id), g_groupManager.TileRowBoundaries(id));
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

// The small icon a window itself advertises via WM_GETICON (falling
// back to the window class's icon, then to the large icon if no small
// one exists) -- the same lookup order Explorer/the taskbar use.
// Returned handles are borrowed from their owning window/class; never
// destroy them.
HICON GetWindowIconHandle(HWND hwnd) {
    HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL2, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    return icon;
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
        icons.push_back(GetWindowIconHandle(member.window));
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
    g_groupManager.RefreshThumbnail(member.window);
    g_groupTabThumbnail->ShowFor(g_groupManager.CachedThumbnail(member.window), tabScreenRect);

    // Keep silently improving the shown preview for a bit in case its
    // content was still mid-load -- see kThumbnailStabilizeTimerId's
    // own comment for why (no universal "finished loading" signal
    // exists to just check once instead).
    g_hoveredThumbnailMember = member.window;
    g_hoveredThumbnailTabRect = tabScreenRect;
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
                                      g_hoveredThumbnailTabRect);
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
    polish::LogDebug(std::format(L"[Polish][DIAG] ActivateGroupTab id={} index={}", id, index));
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

// Called from GroupChromeWindow's "Switch to Tab"/"Switch to Tile"
// context-menu item: flips the group's mode, updates the chrome's own
// rendering to match, and reflows -- ApplyLayout already branches on
// GroupState::Mode(), so switching modes needs no special-casing beyond
// that single flag flip plus a reflow. v1 has no per-mode saved
// geometry (a tiled member just gets repositioned into the shared tab
// rect if switching to Tab, and vice versa) -- acceptable for v1, not
// worth the complexity of remembering "where it would have been."
void ToggleGroupMode(polish::GroupId id) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    const polish::GroupMode newMode =
        (group->Mode() == polish::GroupMode::Tab) ? polish::GroupMode::Tile : polish::GroupMode::Tab;
    group->SetMode(newMode);
    chromeIt->second->SetMode(newMode);
    ReflowGroupTo(id);
    polish::LogDebug(std::format(L"[Polish] Group: mode switched to {} for group id={}",
                                  newMode == polish::GroupMode::Tile ? L"Tile" : L"Tab", id));
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
    const auto selection = picker.ShowModal(owner);
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
        memberIcons.push_back(GetWindowIconHandle(hwnd));
    }

    auto chrome = std::make_unique<polish::GroupChromeWindow>(GetModuleHandleW(nullptr));
    chrome->Show(memberTitles);
    SetWindowTextW(chrome->Handle(), selection->name.c_str());
    chrome->SetMemberIcons(memberIcons);
    chrome->SetOnTabClicked([id](size_t index) { ActivateGroupTab(id, index); });
    chrome->SetOnTabReordered([id](size_t from, size_t to) { ReorderGroupTab(id, from, to); });
    chrome->SetOnModeToggleRequested([id]() { ToggleGroupMode(id); });
    chrome->SetOnEditWindowsRequested([id]() { EditGroupWindows(id); });
    chrome->SetOnResized([id]() { ReflowGroupTo(id); });
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
                L"Alt+Tab (skip minimized)");
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
            }
            return 0;

        case polish::TrayIcon::kCallbackMessage:
            if (g_trayIcon) {
                g_trayIcon->HandleCallbackMessage(lParam);
            }
            return 0;

        case polish::AltTabHook::kHookMessage:
            if (g_altTabHook) {
                g_altTabHook->HandleHookMessage(wParam);
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

        case kCloseGroupMessage:
            // Deferred from CloseGroup -- see its own comment for why
            // this can't safely happen synchronously from within the
            // closing chrome's own WM_CLOSE handling. By now WM_CLOSE/
            // WM_DESTROY have both fully finished and members have
            // already been released, so erasing (and thereby
            // destroying, via ~GroupChromeWindow) is safe here.
            g_groupChromeWindows.erase(static_cast<polish::GroupId>(wParam));
            return 0;

        case WM_DESTROY:
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
            g_trayIcon.reset();
            g_altTabHook.reset();
            g_altTabOverlays.clear();
            g_altTabHighlightBorder.reset();
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

    g_settings = polish::LoadSettings();
    polish::LogDebug(std::format(L"[Polish] settings loaded: restoreSyncEnabled={} altTabEnabled={}",
                                  g_settings.restoreSyncEnabled, g_settings.altTabEnabled));

    g_messageWindow = CreateMessageWindow(instance);
    if (g_messageWindow == nullptr) {
        CloseHandle(singleInstanceMutex);
        return 1;
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

    g_altTabHook = std::make_unique<polish::AltTabHook>(g_messageWindow, AltTabHasEligibleCandidates,
                                                         OnAltTabCycle, OnAltTabCommit, OnAltTabCancel);
    if (!g_altTabHook->IsInstalled()) {
        polish::LogDebug(std::format(L"[Polish] WARNING: failed to install the Alt+Tab keyboard hook. "
                                      L"GetLastError={}",
                                      GetLastError()));
    } else {
        polish::LogDebug(L"[Polish] Alt+Tab keyboard hook installed successfully");
    }

    // Pre-create the dim overlays and highlight border now, at startup,
    // rather than paying CreateWindowExW + first-paint latency in
    // response to the user's actual first Tab press -- confirmed as a
    // real, visible glitch (the previously-active window briefly still
    // looked highlighted/undimmed before the real highlight caught up).
    // EnsureAltTabOverlayPoolSize only ever grows the pool, so this is
    // purely a head start for the common case, not a hard requirement --
    // it still grows safely later if more windows open than were open
    // right now.
    RebuildAltTabCandidates();
    EnsureAltTabOverlayPoolSize(g_altTabCandidates.size());
    EnsureAltTabHighlightBorder();

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
    return static_cast<int>(msg.wParam);
}

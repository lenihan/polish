#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hook/AltTabDimOverlay.h"
#include "hook/AltTabHighlightBorder.h"
#include "hook/AltTabHook.h"
#include "hook/GroupChromeWindow.h"
#include "hook/GroupPickerWindow.h"
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

// Forward-declared: defined near the rest of the group-management code
// (TriggerNewGroup etc.), further down; called from OnForegroundChanged
// above that point.
void SyncGroupFromForeground(HWND hwnd);

// Forward-declared for the same reason as SyncGroupFromForeground
// above; called from OnWinEvent's EVENT_OBJECT_NAMECHANGE case. No-op
// unless hwnd is a group member -- a member's title changing is common
// (browser tabs, unsaved-changes markers, etc.) and previously just sat
// stale in its group's tab label until something else (a tab click, a
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

void OnForegroundChanged(HWND newForeground) {
    if (newForeground == g_trackedWindow) {
        return;
    }
    KillTimer(g_messageWindow, kSettleTimerId);
    g_inMoveSizeLoop = false;
    g_pendingSettleRect.reset();
    g_trackedWindow = nullptr;

    const bool candidate = polish::IsCandidateWindow(newForeground);
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

    // Unconditional on `candidate` (though a group member always is
    // one) -- a member activated some other way than its own group's
    // tab strip (e.g. directly via Alt+Tab) still needs its group's
    // active-tab state/chrome synced to match. No-op if newForeground
    // isn't a member of any group.
    SyncGroupFromForeground(newForeground);

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
constexpr UINT kMenuIdStartAtLogin = 4;
constexpr UINT kMenuIdAbout = 5;
constexpr UINT kMenuIdExit = 6;

constexpr wchar_t kAboutUrl[] = L"https://www.linkedin.com/in/davidlenihan/";

// Global hotkey id for RegisterHotKey/WM_HOTKEY -- Win+Alt+G ("Group"),
// user-chosen. No existing hotkey infrastructure to reuse (the old
// HotkeyManager was deleted earlier in this project's history); a
// single RegisterHotKey call is simple enough not to need one of its
// own, unlike Alt+Tab's WH_KEYBOARD_LL hook (there's no native OS
// behavior to suppress here, just one combination to claim).
constexpr int kNewGroupHotkeyId = 1;

// Repositions/promotes group id's members into `contentRect` -- the
// single path both the initial layout (TriggerNewGroup) and every
// later reflow (a tab click, a group-drag, a member activated
// externally via Alt+Tab) go through, so they can never drift out of
// sync with each other.
//
// If ApplyLayout reports that some member wouldn't fit -- a real,
// confirmed case: a window's own declared minimum size (Outlook is a
// real example a user hit) can be larger than contentRect, and
// SetWindowPos silently clamps to it rather than failing -- the chrome
// is grown to fit and layout is re-applied once, so that member ends up
// correctly filling its (now larger) share of the group instead of
// visibly spilling outside it.
void ReflowGroupTo(polish::GroupId id, const RECT& contentRect) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    if (group == nullptr) {
        return;
    }
    const SIZE needed = g_groupManager.ApplyLayout(*group, contentRect);
    const int requestedWidth = contentRect.right - contentRect.left;
    const int requestedHeight = contentRect.bottom - contentRect.top;
    if (needed.cx <= requestedWidth && needed.cy <= requestedHeight) {
        return;
    }

    auto chromeIt = g_groupChromeWindows.find(id);
    if (chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    polish::LogDebug(std::format(
        L"[Polish] Group: member(s) wouldn't fit group id={} (requested {}x{}, needed {}x{}) -- growing chrome",
        id, requestedWidth, requestedHeight, needed.cx, needed.cy));
    chromeIt->second->GrowContentAreaTo(needed);
    g_groupManager.ApplyLayout(*group, chromeIt->second->ContentRectInScreenCoords());
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
}

// Called from OnForegroundChanged for every foreground change (see the
// forward declaration near the group globals for why): if hwnd is a
// member of some group, syncs that group's active index and its
// chrome's visual highlight to match, and reflows so the now-active
// member is promoted to front within the shared content rect -- the
// case this exists for is hwnd becoming foreground via something other
// than its own group's tab strip (e.g. Alt+Tab), which ActivateGroupTab
// above never sees. Deliberately does *not* call SetForegroundWindow --
// hwnd is already foreground, that's what triggered this.
void SyncGroupFromForeground(HWND hwnd) {
    polish::GroupState* group = g_groupManager.FindGroupContaining(hwnd);
    if (group == nullptr) {
        return;
    }
    group->SetActiveWindow(hwnd);

    auto chromeIt = g_groupChromeWindows.find(group->Id());
    if (chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    if (const auto activeIndex = group->ActiveIndex(); activeIndex.has_value()) {
        chromeIt->second->SetActiveIndex(*activeIndex);
    }
    ReflowGroupTo(group->Id(), chromeIt->second->ContentRectInScreenCoords());
    polish::LogDebug(std::format(L"[Polish] Group: member hwnd={} activated externally, synced group id={}",
                                  reinterpret_cast<void*>(hwnd), group->Id()));
}

// Called when a group's tab strip is clicked (GroupChromeWindow's
// onTabClicked callback): switches which member is active, both in the
// pure GroupState and the chrome's own visual highlight, reflows (so
// the newly active member is promoted to front), and actually focuses
// it -- unlike Alt+Tab's highlight-preview promote (SWP_NOACTIVATE,
// deliberately not stealing focus while cycling), a tab click is a
// deliberate "switch to this" action that should behave like clicking
// any other window.
void ActivateGroupTab(polish::GroupId id, size_t index) {
    polish::GroupState* group = g_groupManager.FindGroup(id);
    auto chromeIt = g_groupChromeWindows.find(id);
    if (group == nullptr || chromeIt == g_groupChromeWindows.end()) {
        return;
    }
    group->SetActiveIndex(index);
    chromeIt->second->SetActiveIndex(index);
    ReflowGroupTo(id, chromeIt->second->ContentRectInScreenCoords());

    if (const auto active = group->ActiveWindow(); active.has_value() && IsWindow(*active)) {
        InjectHarmlessCtrlKeystroke();
        SetForegroundWindow(*active);
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
    ReflowGroupTo(id, chromeIt->second->ContentRectInScreenCoords());
    polish::LogDebug(std::format(L"[Polish] Group: mode switched to {} for group id={}",
                                  newMode == polish::GroupMode::Tile ? L"Tile" : L"Tab", id));
}

// Called from GroupChromeWindow's "Edit windows..." context-menu item:
// reopens the picker pre-checked with the group's current membership
// and mode, then diffs the confirmed selection against current
// membership -- newly unchecked windows are removed, newly checked ones
// added (GroupState::Remove/AddWindow already handle active-index
// bookkeeping and duplicate/no-op safety, so this is purely a diff, no
// new membership logic needed here).
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
    const auto result = picker.ShowModal(chromeIt->second->Handle(), currentMembers, group->Mode(), true);
    if (!result.has_value()) {
        polish::LogDebug(L"[Polish] Group: edit-windows picker cancelled");
        return;
    }

    for (HWND hwnd : currentMembers) {
        if (std::find(result->windows.begin(), result->windows.end(), hwnd) == result->windows.end()) {
            group->Remove(hwnd);
        }
    }
    for (HWND hwnd : result->windows) {
        group->AddWindow(hwnd);
    }
    if (group->Mode() != result->mode) {
        group->SetMode(result->mode);
        chromeIt->second->SetMode(result->mode);
    }

    chromeIt->second->SetMemberTitles(CollectMemberTitles(*group));
    if (const auto activeIndex = group->ActiveIndex(); activeIndex.has_value()) {
        chromeIt->second->SetActiveIndex(*activeIndex);
    }
    ReflowGroupTo(id, chromeIt->second->ContentRectInScreenCoords());
    polish::LogDebug(std::format(L"[Polish] Group: edited group id={}, now {} window(s), mode={}", id,
                                  group->MemberCount(), group->Mode() == polish::GroupMode::Tile ? L"Tile" : L"Tab"));
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
    const polish::GroupId id = g_groupManager.CreateGroup(selection->windows, selection->mode);
    polish::LogDebug(std::format(L"[Polish] New Group: created group id={} with {} window(s), mode={}", id,
                                  selection->windows.size(),
                                  selection->mode == polish::GroupMode::Tile ? L"Tile" : L"Tab"));
    std::vector<std::wstring> memberTitles;
    for (HWND hwnd : selection->windows) {
        wchar_t title[256] = L"";
        GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
        polish::LogDebug(
            std::format(L"[Polish] New Group: member hwnd={} title=\"{}\"", reinterpret_cast<void*>(hwnd), title));
        memberTitles.emplace_back(title);
    }

    auto chrome = std::make_unique<polish::GroupChromeWindow>(GetModuleHandleW(nullptr));
    chrome->Show(memberTitles, selection->mode);
    chrome->SetOnTabClicked([id](size_t index) { ActivateGroupTab(id, index); });
    chrome->SetOnTabReordered([id](size_t from, size_t to) { ReorderGroupTab(id, from, to); });
    chrome->SetOnModeToggleRequested([id]() { ToggleGroupMode(id); });
    chrome->SetOnEditWindowsRequested([id]() { EditGroupWindows(id); });
    chrome->SetOnMoved([id](const RECT& newContentRect) { ReflowGroupTo(id, newContentRect); });
    polish::LogDebug(std::format(L"[Polish] New Group: chrome window created hwnd={} for group id={}",
                                  reinterpret_cast<void*>(chrome->Handle()), id));

    ReflowGroupTo(id, chrome->ContentRectInScreenCoords());
    g_groupChromeWindows[id] = std::move(chrome);
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
    AppendMenuW(menu, MF_STRING, kMenuIdNewGroup, L"New Group\tWin+Alt+G");
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
            g_groupChromeWindows.clear();
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

    if (!RegisterHotKey(g_messageWindow, kNewGroupHotkeyId, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'G')) {
        polish::LogDebug(
            std::format(L"[Polish] WARNING: failed to register the Win+Alt+G hotkey. GetLastError={}",
                        GetLastError()));
    } else {
        polish::LogDebug(L"[Polish] Win+Alt+G (New Group) hotkey registered successfully");
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

#include <windows.h>

#include <format>
#include <memory>
#include <optional>
#include <string>

#include "tray/TrayIcon.h"
#include "util/Logging.h"
#include "windowtracking/RectUtils.h"

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
UINT g_taskbarCreatedMessage = 0;

std::unique_ptr<polish::TrayIcon> g_trayIcon;

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

// Whether hwnd looks like a normal top-level application window worth
// tracking at all -- mirrors the heuristic the taskbar/Alt-Tab use.
// Filters out tooltips, popups, IME windows, etc.
bool IsCandidateWindow(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0) {
        return false;
    }
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == 0) {
        return false;
    }
    return true;
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

    const bool candidate = IsCandidateWindow(newForeground);
    wchar_t title[256] = L"";
    GetWindowTextW(newForeground, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
    polish::LogDebug(std::format(L"[Polish] foreground changed: hwnd={} title=\"{}\" candidate={}",
                                  reinterpret_cast<void*>(newForeground), title, candidate));

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
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd == g_trackedWindow) {
                g_trackedWindow = nullptr;
                g_inMoveSizeLoop = false;
                g_pendingSettleRect.reset();
                KillTimer(g_messageWindow, kSettleTimerId);
            }
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

        case WM_COMMAND:
            if (g_trayIcon) {
                g_trayIcon->HandleCommand(wParam);
            }
            return 0;

        case WM_DESTROY:
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
            g_trayIcon.reset();
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

    g_trayIcon = std::make_unique<polish::TrayIcon>(g_messageWindow, [] { DestroyWindow(g_messageWindow); });

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

#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "tabs/TabSwitching.h"
#include "windowtracking/TaskbarButtons.h"

namespace polish {

// Posted to the notify window when an enumeration finishes. WPARAM is the
// generation of the request that produced it (see RequestTabs) -- compare
// it against the generation you asked for and drop anything older, since
// a slow enumeration can land after a newer one has already superseded
// it. LPARAM is unused.
inline constexpr UINT kTabsReadyMessage = WM_APP + 0x51;

// Posted when a selection-rect request finishes (see RequestSelectionRect).
// WPARAM is that request's generation; LPARAM is unused. Sent whether or
// not a selection was found, so a caller waiting on one is never left
// hanging by an app that exposes no text at all.
inline constexpr UINT kSelectionReadyMessage = WM_APP + 0x52;

// Posted when a taskbar-button enumeration finishes (see
// RequestTaskbarButtons). WPARAM is that request's generation; LPARAM is
// unused. Sent even when no buttons were found, so a caller can tell
// "the taskbar has nothing on it" from "the query never came back" and
// disable itself rather than wait forever.
inline constexpr UINT kTaskbarButtonsReadyMessage = WM_APP + 0x53;

// Owns the process's single UI Automation client, and the thread every
// UIA call runs on. Three unrelated jobs ride on it rather than paying
// for a thread and a UIA client each: reading a window's document tabs
// for Alt+` (see TabSwitching.h), finding the bounding box of the current
// text selection so the copy/paste bullseye can aim at what was actually
// selected, and reading the taskbar's app buttons (see TaskbarButtons.h).
//
// Why a thread of its own, rather than calling UIA where it is needed:
//
//   - Never the hook thread. A low-level keyboard hook that takes too
//     long is silently unhooked by Windows (LowLevelHooksTimeout), which
//     would break Alt+Tab and every other key this app watches -- not
//     just tab switching. A cross-process UIA call measured ~50ms against
//     VS Code and is unbounded in the general case, so it is nowhere near
//     safe there. See AltTabHook's class comment.
//   - Never the UI thread. That thread paints the switcher overlays and
//     the panel; a 50ms stall in it is a visible hitch in exactly the
//     latency-sensitive path this feature lives on.
//
// The thread is COM-initialized MTA so its UIA calls need no marshalling,
// and it exclusively owns the IUIAutomationElement handles for the tabs
// it last enumerated. Those handles never cross a thread boundary -- the
// rest of the app refers to a tab only by its index in the last snapshot,
// which is what keeps this class's public surface free of COM entirely.
class UiaWorker {
public:
    explicit UiaWorker(HWND notifyWindow);
    ~UiaWorker();

    UiaWorker(const UiaWorker&) = delete;
    UiaWorker& operator=(const UiaWorker&) = delete;

    // Queue an enumeration of `window`'s document tabs under `rule`.
    // Returns immediately with the generation stamped on this request;
    // the result arrives later as kTabsReadyMessage at the notify window,
    // and is readable via Snapshot(). A request supersedes any still
    // queued, so a burst of foreground changes costs one enumeration.
    uint64_t RequestTabs(HWND window, const TabRule& rule);

    // Queue "where is the current text selection?", resolved against
    // whatever has keyboard focus at the moment the worker gets to it.
    // Returns the generation stamped on this request; the answer arrives
    // as kSelectionReadyMessage and is readable via LatestSelection().
    //
    // Measured at 4-8ms against VS Code, far quicker than reading tabs,
    // but it is still a cross-process call with no bounded worst case --
    // which is why it belongs here rather than inline on the UI thread.
    uint64_t RequestSelectionRect();

    // Queue "what app buttons are on the taskbar right now, and where?".
    // Returns the generation stamped on this request; the answer arrives
    // as kTaskbarButtonsReadyMessage and is readable via
    // LatestTaskbarButtons().
    //
    // Measured at ~15ms warm on a 7-button taskbar, which is far too slow
    // to run per mouse-move but trivial as a cached snapshot -- so the
    // caller holds the result and re-requests only when the taskbar
    // actually changes (UIA structure/bounds events, WM_DISPLAYCHANGE,
    // explorer restarting), with a low-frequency timer as a safety net.
    uint64_t RequestTaskbarButtons();

    // Queue "make the tab at `index` in generation `generation` the
    // frontmost one". Ignored by the worker if its cached elements have
    // moved on to a newer generation, so a commit can never activate a
    // tab the user was not actually looking at.
    void RequestActivate(uint64_t generation, size_t index);

    struct Snapshot {
        HWND window = nullptr;
        std::vector<TabTarget> tabs;
        uint64_t generation = 0;
        // False until the first enumeration for this window has finished.
        // Distinguishes "this app has no tabs" from "not asked yet", which
        // the eligibility check needs to tell apart.
        bool resolved = false;
    };

    // The most recent completed enumeration. Cheap and lock-guarded, so
    // it is safe to call from the UI thread on every cycle.
    Snapshot LatestSnapshot() const;

    struct SelectionSnapshot {
        // Union of every bounding rectangle the selection reported, in
        // screen coordinates. A selection spanning several lines gives one
        // rect per line; the union is what "the middle of what I selected"
        // means for a caller aiming at it.
        RECT bounds{};
        bool found = false;  // false when nothing is selected, or the focused control exposes no text

        // Where the text caret is, via MSAA's OBJID_CARET, resolved in the
        // same trip and used when there is no selection to aim at.
        //
        // A second mechanism rather than UI Automation's own caret because
        // UIA's is not dependable: Chromium reports a fixed, wrong caret
        // rect through TextPattern (docs/LIMITATIONS.md #21). MSAA exposes
        // the caret as an object in its own right, which is how screen
        // magnifiers follow it in Chrome, and covers apps that draw their
        // own caret and expose no Win32 one.
        RECT caret{};
        bool caretFound = false;

        // The caret as UI Automation reports it (a collapsed selection
        // range). The least trusted of the three, and only consulted when
        // caretFound is false: Chromium fills this in with a fixed wrong
        // value, while MSAA answers correctly there, so in the one app
        // known to lie it is never reached. It exists for the opposite
        // case -- XAML controls such as Explorer's address bar, which sit
        // behind an InputSiteWindowClass host that returns S_FALSE for
        // OBJID_CARET but do implement TextPattern.
        RECT uiaCaret{};
        bool uiaCaretFound = false;

        uint64_t generation = 0;
    };
    SelectionSnapshot LatestSelection() const;

    struct TaskbarSnapshot {
        std::vector<TaskbarButton> buttons;
        uint64_t generation = 0;
        // False until the first enumeration has finished. Distinguishes
        // "the taskbar genuinely has no app buttons" from "not asked
        // yet" -- the same distinction Snapshot::resolved draws, and for
        // the same reason: a feature that fails closed needs to know
        // which it is looking at.
        bool resolved = false;
        // False when the taskbar could not be found or read at all (no
        // Shell_TrayWnd, no TaskbarFrame, UIA refused). The signal to
        // disable the taskbar features on this machine and leave the
        // shell completely alone, rather than act on a half-read tree.
        bool taskbarUsable = false;
    };

    // The most recent completed taskbar enumeration. Cheap and
    // lock-guarded. NOT safe to call from a low-level hook callback --
    // it takes a lock the worker thread also holds, and a hook that
    // blocks is silently unhooked; the hook reads its own copy instead.
    TaskbarSnapshot LatestTaskbarButtons() const;

private:
    struct Request {
        enum class Kind { Enumerate, Activate, SelectionRect, TaskbarButtons } kind = Kind::Enumerate;
        HWND window = nullptr;
        TabRule rule;
        uint64_t generation = 0;
        size_t index = 0;
    };

    void ThreadMain();
    // Both run on the worker thread only, with COM live.
    void Enumerate(const Request& request);
    void Activate(const Request& request);
    void ResolveSelection(const Request& request);
    void EnumerateTaskbarButtons(const Request& request);

    HWND notifyWindow_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Request> queue_;
    bool stopping_ = false;
    std::atomic<uint64_t> nextGeneration_{1};
    Snapshot snapshot_;                    // guarded by mutex_
    SelectionSnapshot selectionSnapshot_;  // guarded by mutex_
    TaskbarSnapshot taskbarSnapshot_;      // guarded by mutex_

    // Worker-thread-only state. Declared here rather than as locals so the
    // enumerated elements survive between an Enumerate and the Activate
    // that follows it; never touched under mutex_, never read elsewhere.
    struct WorkerState;
    WorkerState* state_ = nullptr;
};

}  // namespace polish

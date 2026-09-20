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

namespace polish {

// Posted to the notify window when an enumeration finishes. WPARAM is the
// generation of the request that produced it (see RequestTabs) -- compare
// it against the generation you asked for and drop anything older, since
// a slow enumeration can land after a newer one has already superseded
// it. LPARAM is unused.
inline constexpr UINT kTabsReadyMessage = WM_APP + 0x51;

// Owns the process's single UI Automation client, and the thread every
// UIA call runs on.
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
class UiaTabWorker {
public:
    explicit UiaTabWorker(HWND notifyWindow);
    ~UiaTabWorker();

    UiaTabWorker(const UiaTabWorker&) = delete;
    UiaTabWorker& operator=(const UiaTabWorker&) = delete;

    // Queue an enumeration of `window`'s document tabs under `rule`.
    // Returns immediately with the generation stamped on this request;
    // the result arrives later as kTabsReadyMessage at the notify window,
    // and is readable via Snapshot(). A request supersedes any still
    // queued, so a burst of foreground changes costs one enumeration.
    uint64_t RequestTabs(HWND window, const TabRule& rule);

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

private:
    struct Request {
        enum class Kind { Enumerate, Activate } kind = Kind::Enumerate;
        HWND window = nullptr;
        TabRule rule;
        uint64_t generation = 0;
        size_t index = 0;
    };

    void ThreadMain();
    // Both run on the worker thread only, with COM live.
    void Enumerate(const Request& request);
    void Activate(const Request& request);

    HWND notifyWindow_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Request> queue_;
    bool stopping_ = false;
    std::atomic<uint64_t> nextGeneration_{1};
    Snapshot snapshot_;  // guarded by mutex_

    // Worker-thread-only state. Declared here rather than as locals so the
    // enumerated elements survive between an Enumerate and the Activate
    // that follows it; never touched under mutex_, never read elsewhere.
    struct WorkerState;
    WorkerState* state_ = nullptr;
};

}  // namespace polish

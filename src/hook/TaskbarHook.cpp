#include "hook/TaskbarHook.h"

namespace polish {

namespace {

// Same reasoning as AltTabHook's own g_instance: low-level hooks take no
// user-data parameter, and exactly one of these exists for the app's
// lifetime. Separate from that one on purpose -- see the class comment.
TaskbarHook* g_instance = nullptr;

// How far the system's last input may be ahead of this hook's last event
// before the hook is presumed dead. Generous, because the cost of being
// wrong in the other direction is silence: a hook that Windows dropped
// never comes back on its own, and the user would see the taskbar quietly
// revert to native behavior with nothing in the log.
constexpr ULONGLONG kPresumedDeadMs = 5000;

bool IsCtrlHeld() { return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; }

// True while any mouse button is physically down. GetAsyncKeyState's high
// bit is the live physical state, which is what this needs -- the
// per-queue GetKeyState would answer as of the last message this thread
// processed, and the point of asking is to be ahead of that.
bool AnyMouseButtonDown() {
    constexpr int kButtons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int key : kButtons) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            return true;
        }
    }
    return false;
}

bool IsShiftHeld() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

bool IsButtonDownMessage(WPARAM message) {
    return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
           message == WM_XBUTTONDOWN;
}

bool IsButtonUpMessage(WPARAM message) {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
           message == WM_XBUTTONUP;
}

// Stamped on every press this hook injects, and checked on the way in, so
// a replayed press is never mistaken for the user's and replayed again --
// which would be an unbounded loop inside a low-level hook, the worst
// place in the app for one.
//
// A signature rather than LLMHF_INJECTED, which would also ignore input
// from any other injecting process: the test harnesses that drive this
// feature all use SendInput, and a hook that ignored injected input could
// only ever be tested by hand.
constexpr ULONG_PTR kReplaySignature = 0x504F4C53;  // "POLS"

// Re-sends a button press the shield swallowed, now that the shield has
// been opened for it.
//
// Needed because opening the shield does not help the press that opened
// it. Measured three ways against a live taskbar:
//
//   - WS_EX_TRANSPARENT set from inside the hook callback, press passed
//     through: nothing happens. The press still lands on the shield.
//   - A watch across a real press: the shield goes pass-through 4ms in
//     and the point under the cursor hit-tests to MSTaskSwWClass. So the
//     style change itself lands, and lands quickly.
//   - The identical injected press, sent 300ms after the shield opened:
//     the jumplist appears.
//
// So the style is right and the press is right; only the moment is wrong.
// An event generated from inside the callback is routed as though the
// shield were still closed, and the fix is to let the hook return first --
// which is why this is called from the message loop (kReplayPressMessage)
// and not from where the decision is made.
//
// `release` is false for the ordinary case, where only the press is
// replayed and the user's own release reaches the taskbar untouched: that
// is what keeps a press-and-drag onto a taskbar button one continuous
// gesture rather than a synthetic click followed by a stray release. It
// is true only when the user out-ran the message loop -- see
// pendingReplayNeedsRelease_.
void ReplayButtonDown(WPARAM message, DWORD mouseData, bool release) {
    INPUT inputs[2]{};
    DWORD downFlag = 0;
    DWORD upFlag = 0;
    switch (message) {
        case WM_LBUTTONDOWN:
            downFlag = MOUSEEVENTF_LEFTDOWN;
            upFlag = MOUSEEVENTF_LEFTUP;
            break;
        case WM_RBUTTONDOWN:
            downFlag = MOUSEEVENTF_RIGHTDOWN;
            upFlag = MOUSEEVENTF_RIGHTUP;
            break;
        case WM_MBUTTONDOWN:
            downFlag = MOUSEEVENTF_MIDDLEDOWN;
            upFlag = MOUSEEVENTF_MIDDLEUP;
            break;
        case WM_XBUTTONDOWN:
            downFlag = MOUSEEVENTF_XDOWN;
            upFlag = MOUSEEVENTF_XUP;
            break;
        default:
            return;
    }
    UINT count = 0;
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dwFlags = downFlag;
    inputs[count].mi.dwExtraInfo = kReplaySignature;
    // Which of the two X buttons, carried in the high word of
    // MSLLHOOKSTRUCT::mouseData and wanted whole in mi.mouseData.
    inputs[count].mi.mouseData = (message == WM_XBUTTONDOWN) ? HIWORD(mouseData) : 0;
    ++count;
    if (release) {
        inputs[count] = inputs[count - 1];
        inputs[count].mi.dwFlags = upFlag;
        ++count;
    }
    // No movement alongside: the cursor is already exactly where the user
    // put it, and a synthetic move to the same point risks being coalesced
    // away or landing a pixel off on a scaled display.
    SendInput(count, inputs, sizeof(INPUT));
}

}  // namespace

TaskbarHook::TaskbarHook(HWND messageWindow, std::function<void(uint64_t, int)> onHoverChanged,
                          std::function<void(uint64_t, int, ClickAction)> onCycleClick,
                          std::function<void(bool)> setPassThrough)
    : messageWindow_(messageWindow),
      onHoverChanged_(std::move(onHoverChanged)),
      onCycleClick_(std::move(onCycleClick)),
      setPassThrough_(std::move(setPassThrough)) {
    g_instance = this;
    Install();
}

TaskbarHook::~TaskbarHook() {
    Uninstall();
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

bool TaskbarHook::Install() {
    if (hook_ == nullptr) {
        hook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
        lastEventTick_ = GetTickCount64();
    }
    return hook_ != nullptr;
}

void TaskbarHook::Uninstall() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
}

bool TaskbarHook::EnsureInstalled() {
    if (hook_ == nullptr) {
        return Install();
    }
    LASTINPUTINFO lastInput{};
    lastInput.cbSize = sizeof(lastInput);
    if (!GetLastInputInfo(&lastInput)) {
        return false;
    }
    // dwTime is a 32-bit GetTickCount value, and GetTickCount64's low 32
    // bits are that same counter -- so the two are comparable once both
    // are narrowed. The difference is read back as signed, the standard
    // rollover-safe tick comparison: it stays correct across the 49-day
    // wrap, and it also reads negative (rather than as an enormous
    // positive) in the ordinary startup case where the last system input
    // predates this hook being installed.
    const auto elapsed = static_cast<LONG>(lastInput.dwTime - static_cast<DWORD>(lastEventTick_));
    if (elapsed < static_cast<LONG>(kPresumedDeadMs)) {
        return false;
    }
    Uninstall();
    return Install();
}

void TaskbarHook::SetTargets(uint64_t generation, std::vector<Target> targets) {
    // No lock, deliberately. A low-level hook runs on the thread that
    // installed it, which is the same thread that calls this, so the
    // callback can never be mid-read here. Taking a lock in the callback
    // is what the watchdog above exists to recover from.
    targets_ = std::move(targets);
    generation_ = generation;
    // The button under the pointer may now be a different app, or gone.
    // Forgetting the old index means the next move re-reports whatever is
    // there, rather than staying silent because the index happens to
    // match.
    hoveredIndex_ = -1;
}

LRESULT CALLBACK TaskbarHook::LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance != nullptr) {
        const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (data->dwExtraInfo == kReplaySignature) {
            // One of our own replayed presses on its way to the taskbar.
            // Passing it straight through is what stops the loop.
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        if (g_instance->HandleMouseEvent(wParam, data->pt, data->mouseData)) {
            return 1;  // swallowed: either claimed as a cycle, or replayed
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

int TaskbarHook::HitTest(POINT screenPt) const {
    for (size_t i = 0; i < targets_.size(); ++i) {
        // PtInRect is exclusive on right/bottom, matching
        // HitTestTaskbarButton -- adjacent buttons share an edge, and an
        // inclusive test would give that column of pixels to both.
        if (PtInRect(&targets_[i].rect, screenPt)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TaskbarHook::ApplyPassThrough(bool on) {
    if (setPassThrough_) {
        setPassThrough_(on);
    }
}

bool TaskbarHook::PassThroughWanted() const {
    POINT cursor{};
    if (!GetCursorPos(&cursor) || HitTest(cursor) < 0) {
        // Off the strip there is nothing to hand over, and leaving the
        // shield open would greet the pointer's return with the native
        // flyout. See the same check in HandleMouseEvent.
        return false;
    }
    // Ctrl first, and on its own: the escape hatch has to restore the
    // native flyout too, not merely stop Polish acting, so it hands the
    // strip back even with no button down.
    return IsCtrlHeld() || AnyMouseButtonDown();
}

bool TaskbarHook::HandleMouseEvent(WPARAM message, POINT screenPt, DWORD mouseData) {
    lastEventTick_ = GetTickCount64();

    // A swallowed press always gets its release swallowed, even if Ctrl
    // went down in between or the targets were replaced mid-click.
    // Otherwise the taskbar sees an unpaired button-up and activates the
    // app itself, which is exactly what eating the press was for.
    if (message == WM_LBUTTONUP && swallowedLeftDown_) {
        swallowedLeftDown_ = false;
        return true;
    }
    // Same pairing rule for the swallowed empty-space right-press: an
    // unpaired button-up would open the context menu the press avoided.
    if (message == WM_RBUTTONUP && swallowedRightDown_) {
        swallowedRightDown_ = false;
        return true;
    }

    const bool ctrlHeld = IsCtrlHeld();
    const int index = HitTest(screenPt);

    // The press Polish claims: a left-click on an app with somewhere to
    // go, with or without a modifier. Both modifiers mean something here
    // -- see the class comment on what each takes over.
    const bool claimingThisPress =
        message == WM_LBUTTONDOWN && index >= 0 && targets_[static_cast<size_t>(index)].cyclesOnClick;
    if (claimingThisPress) {
        // Left exactly as Ctrl set it, rather than forced shut: the press
        // is swallowed either way, and slamming the shield closed under a
        // held Ctrl would fight the hover poll, which is about to reopen
        // it on Ctrl's behalf.
        ApplyPassThrough(ctrlHeld);
        swallowedLeftDown_ = true;
        // Ctrl wins when both are down. It is the coarser gesture -- "the
        // other one" rather than "one step back" -- so it is the one a
        // hand fumbling both modifiers more likely meant.
        const UINT claimed = ctrlHeld          ? kToggleClickMessage
                             : IsShiftHeld()   ? kCycleBackClickMessage
                                               : kCycleClickMessage;
        PostMessageW(messageWindow_, claimed, static_cast<WPARAM>(generation_), static_cast<LPARAM>(index));
        return true;
    }

    // The user's release out-ran the message loop: the replay this is
    // waiting on has not been sent yet, so the taskbar has seen no press
    // to end. Swallow the release too and let the replay carry it.
    if (pendingReplayMessage_ != WM_NULL && IsButtonUpMessage(message)) {
        pendingReplayNeedsRelease_ = true;
        return true;
    }

    // Every other press over the strip belongs to the taskbar: the
    // right-click jumplist, shift/middle-click, a drag that starts on a
    // button, a left-click on an app with nothing to cycle, and anything
    // at all while Ctrl is held. Opening the shield is not enough on its
    // own -- see ReplayButtonDown -- so the original is swallowed and a
    // fresh one posted to arrive in its place, a message-loop turn later.
    if (IsButtonDownMessage(message) && index >= 0) {
        ApplyPassThrough(true);
        pendingReplayMessage_ = message;
        pendingReplayMouseData_ = mouseData;
        pendingReplayNeedsRelease_ = false;
        PostMessageW(messageWindow_, kReplayPressMessage, message, static_cast<LPARAM>(mouseData));
        return true;
    }

    // A click on the taskbar that is not over an app button. Ctrl and Alt
    // mean the user is doing something else; Shift is meaningful (it
    // reverses, as it does for Tab). The left press is passed on, the
    // right one swallowed -- see the class comment.
    const bool emptyPress = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) && index < 0 && !ctrlHeld &&
                            (GetKeyState(VK_MENU) & 0x8000) == 0;
    if (emptyPress) {
        const HWND root = GetAncestor(WindowFromPoint(screenPt), GA_ROOT);
        wchar_t className[32] = L"";
        if (root != nullptr && GetClassNameW(root, className, 32) > 0 &&
            (wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0)) {
            const bool right = message == WM_RBUTTONDOWN;
            PostMessageW(messageWindow_, kEmptyClickMessage, (IsShiftHeld() ? 1u : 0u) | (right ? 2u : 0u),
                         MAKELPARAM(static_cast<short>(screenPt.x), static_cast<short>(screenPt.y)));
            if (right) {
                swallowedRightDown_ = true;
                return true;
            }
        }
    }

    // Only ever open over the strip. The shield covers nothing else, so
    // opening it while the pointer is elsewhere buys nothing -- and costs
    // a great deal: it stays open, and the moment the pointer returns to
    // the strip the taskbar gets the hover and its flyout comes back, on
    // top of Polish's own list.
    //
    // Measured: pressing a mouse button anywhere over the hover panel --
    // clicking a row, or one of its buttons -- opened the shield, because
    // this asked only "is a button down" and never "is the pointer over
    // anything the shield covers".
    //
    // AnyMouseButtonDown cannot answer for a press happening right now --
    // the hook runs ahead of the state it would read -- but every other
    // event is safely behind it.
    ApplyPassThrough(index >= 0 && (ctrlHeld || AnyMouseButtonDown()));

    if (message == WM_MOUSEMOVE) {
        // Never swallowed: returning non-zero for a move freezes the
        // cursor outright (measured -- see docs/LIMITATIONS.md #22).
        if (index != hoveredIndex_) {
            hoveredIndex_ = index;
            PostMessageW(messageWindow_, kHoverMessage, static_cast<WPARAM>(generation_),
                         static_cast<LPARAM>(index));
        }
    }
    return false;
}

void TaskbarHook::HandleHookMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kReplayPressMessage) {
        const bool release = pendingReplayNeedsRelease_;
        pendingReplayMessage_ = WM_NULL;
        pendingReplayNeedsRelease_ = false;
        ReplayButtonDown(wParam, static_cast<DWORD>(lParam), release);
        return;
    }
    if (message == kEmptyClickMessage) {
        if (onEmptyClick_) {
            onEmptyClick_(POINT{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))},
                          (wParam & 1) != 0, (wParam & 2) != 0);
        }
        return;
    }
    const auto generation = static_cast<uint64_t>(wParam);
    const auto index = static_cast<int>(static_cast<intptr_t>(lParam));
    if (message == kHoverMessage) {
        if (onHoverChanged_) {
            onHoverChanged_(generation, index);
        }
    } else if (message == kCycleClickMessage || message == kCycleBackClickMessage ||
               message == kToggleClickMessage) {
        if (onCycleClick_) {
            const ClickAction action = message == kToggleClickMessage    ? ClickAction::ToggleRecent
                                       : message == kCycleBackClickMessage ? ClickAction::CycleBackward
                                                                           : ClickAction::CycleForward;
            onCycleClick_(generation, index, action);
        }
    }
}

}  // namespace polish

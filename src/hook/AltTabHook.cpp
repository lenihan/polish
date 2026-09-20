#include "hook/AltTabHook.h"

#include <optional>

namespace polish {

namespace {

// Only one instance is ever installed for the app's lifetime, and
// low-level hooks have no user-data parameter, so a plain static
// pointer is the simplest correct option (same pattern the deleted
// TitleBarMenuHook used).
AltTabHook* g_instance = nullptr;

// RowKeyAction (not "RowAction" -- that name is already
// AltTabHook::RowAction, the *kind* of row action a RowKeyAction message
// carries in its lParam) is the one HookAction value that isn't fully
// self-describing from its tag alone.
// Navigate carries the specific AltTabHook::NavigateStep in lParam, the
// same way RowKeyAction carries its RowAction -- one action rather than
// one per direction, now that there are eight of them.
// PasteChord is unrelated to Alt+Tab sessions -- see SetOnPasteChord.
enum class HookAction : WPARAM { CycleForward, CycleBackward, Commit, Cancel, Navigate, RowKeyAction, PasteChord };

// The navigation key -> step mapping, and the definition of which keys
// count as navigation keys at all (anything this returns nullopt for is
// left completely alone, even mid-session).
std::optional<AltTabHook::NavigateStep> NavigateStepForKey(DWORD vkCode) {
    switch (vkCode) {
        case VK_DOWN:
            return AltTabHook::NavigateStep::Next;
        case VK_UP:
            return AltTabHook::NavigateStep::Prev;
        case VK_PRIOR:
            return AltTabHook::NavigateStep::PageUp;
        case VK_NEXT:
            return AltTabHook::NavigateStep::PageDown;
        case VK_HOME:
            return AltTabHook::NavigateStep::First;
        case VK_END:
            return AltTabHook::NavigateStep::Last;
        case VK_LEFT:
            return AltTabHook::NavigateStep::PrevPanel;
        case VK_RIGHT:
            return AltTabHook::NavigateStep::NextPanel;
        default:
            return std::nullopt;
    }
}

bool IsDown(WPARAM wParam) { return wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN; }
bool IsUp(WPARAM wParam) { return wParam == WM_KEYUP || wParam == WM_SYSKEYUP; }
bool IsShiftKey(DWORD vk) { return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT; }
bool IsAltKey(DWORD vk) { return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU; }
bool IsCtrlKey(DWORD vk) { return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }

// Windows treats a *standalone* Alt press+release (nothing else in
// between) as "focus the menu bar" -- that's what caused the menu-flash
// bug once Tab got swallowed out of the sequence. Injecting a harmless,
// invisible Ctrl tap right after the first swallowed Tab breaks that
// standalone-Alt condition, so the eventual real Alt-up can be let
// through completely untouched (see HandleKeyEvent) instead of being
// swallowed. Swallowing Alt-up was the *previous* mitigation attempt,
// and it caused a worse, real bug: Windows' own internal modifier-state
// tracking never saw Alt actually go up (a swallowed event never reaches
// that layer at all), leaving the whole session believing Alt was still
// held -- surviving even after this process exited, since it's OS-level
// state, not this process's. Never swallow a modifier key-up.
void InjectHarmlessKeystroke() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_CONTROL;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

}  // namespace

AltTabHook::AltTabHook(HWND messageWindow, std::function<Eligibility(SessionKind)> eligibility,
                        std::function<void(bool, SessionKind)> onCycle, std::function<void()> onCommit,
                        std::function<void()> onCancel)
    : messageWindow_(messageWindow),
      eligibility_(std::move(eligibility)),
      onCycle_(std::move(onCycle)),
      onCommit_(std::move(onCommit)),
      onCancel_(std::move(onCancel)) {
    g_instance = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
}

AltTabHook::~AltTabHook() {
    UninstallMouseHook();
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
    }
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

void AltTabHook::InstallMouseHook() {
    if (mouseHook_ == nullptr) {
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
    }
}

void AltTabHook::UninstallMouseHook() {
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

void AltTabHook::EndSession() {
    sessionActive_ = false;
    sessionKind_ = SessionKind::Windows;
    UninstallMouseHook();
}

LRESULT CALLBACK AltTabHook::LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance != nullptr) {
        const auto* data = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (g_instance->HandleKeyEvent(wParam, *data)) {
            return 1;  // swallow: block this event from reaching its target window
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK AltTabHook::LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance != nullptr) {
        const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (g_instance->HandleMouseEvent(wParam, data->pt)) {
            return 1;  // swallow: this click is the commit trigger, not a real interaction
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool AltTabHook::HandleMouseEvent(WPARAM wParam, POINT screenPt) {
    if (!sessionActive_) {
        return false;
    }
    if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN ||
        wParam == WM_XBUTTONDOWN) {
        if (isOwnUI_ && isOwnUI_(screenPt)) {
            // Click landed on Polish's own list-panel UI -- leave it
            // completely alone (not a generic commit trigger) so the
            // panel's own row/button hit-testing handles it instead. See
            // the class comment for why this check is safe to run
            // synchronously here.
            return false;
        }
        // Commits (not cancels), and deliberately NOT swallowed (return
        // false) -- the click should reach whatever's actually under the
        // cursor completely normally (the dim overlays are already
        // WS_EX_TRANSPARENT, so it will), at the same time as ending the
        // session on whatever's currently highlighted. Lets a single
        // click both e.g. minimize some window and land Alt+Tab on
        // whatever was highlighted, which don't have to be the same
        // window.
        EndSession();
        PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::Commit), 0);
    }
    return false;
}

bool AltTabHook::HandleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data) {
    // Track Shift purely from observed hook events, not GetAsyncKeyState
    // -- confirmed via docs that async key state can be stale inside a
    // low-level hook callback (this is explicitly documented for Alt;
    // tracking Shift the same way is the cheap, consistent choice).
    if (IsShiftKey(data.vkCode)) {
        shiftHeld_ = IsDown(wParam);
        return false;
    }
    if (IsCtrlKey(data.vkCode)) {
        ctrlHeld_ = IsDown(wParam);
        return false;
    }

    // Alt-held combos arrive as WM_SYSKEYDOWN/WM_SYSKEYUP, not
    // WM_KEYDOWN/WM_KEYUP -- IsDown/IsUp above already account for that.
    // LLKHF_ALTDOWN, not GetAsyncKeyState, for the same staleness reason.
    const bool altHeld = (data.flags & LLKHF_ALTDOWN) != 0;

    if (data.vkCode == VK_TAB && altHeld && IsDown(wParam)) {
        lastTabDetectedTick_ = GetTickCount64();
        if (nativeHandoffActive_) {
            return false;  // already handed off this Alt-hold, hands off
        }
        if (tabPhysicallyDown_) {
            // OS key-repeat while Tab is physically held, not a genuine
            // new press -- holding Tab down shouldn't rapidly cycle
            // through windows any more than native Alt+Tab does; only a
            // fresh press should advance the highlight. Still swallowed
            // whenever a session is active, so the repeat doesn't leak
            // through to whatever's behind it either.
            return sessionActive_;
        }
        tabPhysicallyDown_ = true;
        if (!sessionActive_) {
            if (ctrlHeld_) {
                // Escape hatch: Ctrl+Alt+Tab bypasses Polish entirely for
                // this Alt-hold. See the class comment.
                nativeHandoffActive_ = true;
                return false;
            }
            // One of the few places this is called synchronously inside
            // the hook -- see the class comment for why that's fine here.
            // Anything but Ready returns false, leaving this keystroke
            // completely untouched so native Alt+Tab handles it normally
            // instead of swallowing into silence with nothing to show
            // for it -- for both "feature off" and "nowhere to go".
            if (!eligibility_ || eligibility_(SessionKind::Windows) != Eligibility::Ready) {
                return false;
            }
            InjectHarmlessKeystroke();  // once per session -- see comment above
            InstallMouseHook();
        }
        // Tab always means "every window": pressing it inside an Alt+`
        // session takes that session back out to windows. No eligibility
        // re-check -- this session only exists because there were at
        // least two windows a moment ago, so it cannot come up empty.
        sessionActive_ = true;
        sessionKind_ = SessionKind::Windows;
        PostMessageW(messageWindow_, kHookMessage,
                     static_cast<WPARAM>(shiftHeld_ ? HookAction::CycleBackward : HookAction::CycleForward),
                     static_cast<LPARAM>(SessionKind::Windows));
        return true;
    }
    if (data.vkCode == VK_TAB && IsUp(wParam)) {
        // Unconditional (not gated on altHeld/sessionActive_) so
        // tabPhysicallyDown_ can never get stuck true -- e.g. Alt could
        // be released fractionally before Tab in some ordering.
        tabPhysicallyDown_ = false;
        return altHeld && sessionActive_;  // swallow the matching up, down was swallowed above
    }

    if (data.vkCode == VK_OEM_3 && altHeld && IsDown(wParam)) {
        // Alt+` -- the same switcher as Alt+Tab, over the foreground
        // window's document tabs. Mirrors Tab's block above, with three
        // differences worth knowing about:
        //  - no native fallback: when there is nothing to switch to the
        //    keystroke is swallowed and nothing is shown (below);
        //  - Ctrl+Alt+` is left completely alone WITHOUT setting
        //    nativeHandoffActive_. There is no native Alt+` to hand off
        //    to, and setting it would only disable Tab for the rest of
        //    the Alt-hold, which is surprising;
        //  - an already-set nativeHandoffActive_ (from Ctrl+Alt+Tab) is
        //    still respected, so a later backtick can't hijack it.
        if (nativeHandoffActive_ || ctrlHeld_) {
            return false;
        }
        if (backtickPhysicallyDown_) {
            return sessionActive_;  // OS key-repeat, not a fresh press
        }
        backtickPhysicallyDown_ = true;

        if (!sessionActive_) {
            switch (eligibility_ ? eligibility_(SessionKind::Tabs) : Eligibility::FeatureDisabled) {
                case Eligibility::FeatureDisabled:
                    return false;  // untouched, exactly as Alt+Tab when off
                case Eligibility::NothingToSwitchTo:
                    // Swallowed, no session. That leaves the OS seeing a
                    // standalone Alt press, so break it once per Alt-hold
                    // -- see nakedAltSuppressedThisHold_.
                    if (!nakedAltSuppressedThisHold_) {
                        nakedAltSuppressedThisHold_ = true;
                        InjectHarmlessKeystroke();
                    }
                    return true;
                case Eligibility::Ready:
                    break;
            }
            InjectHarmlessKeystroke();  // once per session
            InstallMouseHook();
        } else if (sessionKind_ != SessionKind::Tabs) {
            // Switching a live Alt+Tab session over to tabs. The
            // foreground app may expose none, in which case the right
            // answer is "leave the session exactly as it is", not "tear it
            // down" -- swallowed, nothing posted.
            if (eligibility_ == nullptr || eligibility_(SessionKind::Tabs) != Eligibility::Ready) {
                return true;
            }
        }
        sessionActive_ = true;
        sessionKind_ = SessionKind::Tabs;
        PostMessageW(messageWindow_, kHookMessage,
                     static_cast<WPARAM>(shiftHeld_ ? HookAction::CycleBackward : HookAction::CycleForward),
                     static_cast<LPARAM>(SessionKind::Tabs));
        return true;
    }
    if (data.vkCode == VK_OEM_3 && IsUp(wParam)) {
        // Unconditional, same reason as Tab's: it must never get stuck true.
        backtickPhysicallyDown_ = false;
        return altHeld && sessionActive_;  // swallow the matching up, down was swallowed above
    }

    if (IsAltKey(data.vkCode) && IsUp(wParam)) {
        // Unconditional reset (not gated on sessionActive_) so the next
        // fresh Alt-hold always starts clean, regardless of which path
        // (a real session, a native handoff, or neither) this one took.
        nativeHandoffActive_ = false;
        nakedAltSuppressedThisHold_ = false;
        if (sessionActive_) {
            // Deliberately NOT swallowed (return false below) -- see
            // InjectHarmlessKeystroke's comment for why swallowing
            // Alt-up specifically must never happen.
            EndSession();
            PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::Commit), 0);
        }
        return false;
    }

    if (data.vkCode == 'V' || data.vkCode == VK_INSERT) {
        // Purely observational -- never returns true from here, so the
        // paste itself always reaches the target app untouched. Ctrl+V
        // (any Shift state, so Ctrl+Shift+V plain-text paste counts) or
        // Shift+Insert, with Alt not held. Falls through afterward: neither
        // key is used by any block below.
        const bool isV = data.vkCode == 'V';
        bool& physicallyDown = isV ? vPhysicallyDown_ : insertPhysicallyDown_;
        if (IsDown(wParam)) {
            if (!physicallyDown) {
                physicallyDown = true;
                const bool isChord = !altHeld && (isV ? ctrlHeld_ : (shiftHeld_ && !ctrlHeld_));
                if (isChord) {
                    PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::PasteChord), 0);
                }
            }
        } else if (IsUp(wParam)) {
            physicallyDown = false;
        }
    }

    if (sessionActive_) {
        // Strictly gated on sessionActive_ already being true -- unlike
        // Tab, these keys are used constantly system-wide and must never
        // be able to start a session on their own, nor have any effect
        // when Alt+Tab isn't active. See class comment.
        if (const std::optional<AltTabHook::NavigateStep> step = NavigateStepForKey(data.vkCode)) {
            bool& physicallyDown = navigateKeysDown_[static_cast<size_t>(*step)];
            if (IsDown(wParam)) {
                if (physicallyDown) {
                    return true;  // OS key-repeat, not a fresh press -- swallow, don't re-navigate
                }
                physicallyDown = true;
                // Posted, not called directly -- same reason Tab's onCycle_
                // is posted rather than invoked synchronously here: the hook
                // callback must stay trivial (see class comment).
                PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::Navigate),
                             static_cast<LPARAM>(*step));
                return true;
            }
            if (IsUp(wParam)) {
                physicallyDown = false;
                return true;
            }
        }
    }

    if (sessionActive_ && (data.vkCode == VK_DELETE || data.vkCode == VK_OEM_MINUS || data.vkCode == VK_OEM_PLUS)) {
        // Same "must already be in a session, never able to start one"
        // gating and per-key debounce shape as the arrow-key block above.
        bool& physicallyDown = (data.vkCode == VK_DELETE)       ? deletePhysicallyDown_
                                : (data.vkCode == VK_OEM_MINUS) ? minusPhysicallyDown_
                                                                 : plusPhysicallyDown_;
        if (IsDown(wParam)) {
            if (physicallyDown) {
                return true;  // OS key-repeat, not a fresh press -- swallow, don't re-fire
            }
            physicallyDown = true;
            const RowAction action = (data.vkCode == VK_DELETE)       ? RowAction::Close
                                      : (data.vkCode == VK_OEM_MINUS) ? RowAction::MinimizeToggle
                                                                       : RowAction::MaximizeToggle;
            PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::RowKeyAction),
                         static_cast<LPARAM>(action));
            return true;
        }
        if (IsUp(wParam)) {
            physicallyDown = false;
            return true;
        }
    }

    if (data.vkCode == VK_ESCAPE && IsDown(wParam) && sessionActive_) {
        // Ends the session outright (unlike Alt-up above, Escape is safe
        // to swallow -- it's not a modifier, so it carries none of the
        // stuck-state risk). If Alt is still physically held afterward
        // and the user presses Tab again, that correctly starts a fresh
        // session rather than silently doing nothing.
        EndSession();
        PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::Cancel), 0);
        return true;
    }

    return false;
}

void AltTabHook::HandleHookMessage(WPARAM wParam, LPARAM lParam) {
    switch (static_cast<HookAction>(wParam)) {
        case HookAction::CycleForward:
            if (onCycle_) {
                onCycle_(/*backward=*/false, static_cast<SessionKind>(lParam));
            }
            break;
        case HookAction::CycleBackward:
            if (onCycle_) {
                onCycle_(/*backward=*/true, static_cast<SessionKind>(lParam));
            }
            break;
        case HookAction::Commit:
            if (onCommit_) {
                onCommit_();
            }
            break;
        case HookAction::Cancel:
            if (onCancel_) {
                onCancel_();
            }
            break;
        case HookAction::Navigate:
            if (onNavigate_) {
                onNavigate_(static_cast<NavigateStep>(lParam));
            }
            break;
        case HookAction::RowKeyAction:
            if (onRowAction_) {
                onRowAction_(static_cast<RowAction>(lParam));
            }
            break;
        case HookAction::PasteChord:
            if (onPasteChord_) {
                onPasteChord_();
            }
            break;
    }
}

}  // namespace polish

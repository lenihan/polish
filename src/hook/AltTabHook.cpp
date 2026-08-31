#include "hook/AltTabHook.h"

namespace polish {

namespace {

// Only one instance is ever installed for the app's lifetime, and
// low-level hooks have no user-data parameter, so a plain static
// pointer is the simplest correct option (same pattern the deleted
// TitleBarMenuHook used).
AltTabHook* g_instance = nullptr;

enum class HookAction : WPARAM { CycleForward, CycleBackward, Commit, Cancel };

bool IsDown(WPARAM wParam) { return wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN; }
bool IsUp(WPARAM wParam) { return wParam == WM_KEYUP || wParam == WM_SYSKEYUP; }
bool IsShiftKey(DWORD vk) { return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT; }
bool IsAltKey(DWORD vk) { return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU; }

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

AltTabHook::AltTabHook(HWND messageWindow, std::function<void(bool)> onCycle,
                        std::function<void()> onCommit, std::function<void()> onCancel)
    : messageWindow_(messageWindow),
      onCycle_(std::move(onCycle)),
      onCommit_(std::move(onCommit)),
      onCancel_(std::move(onCancel)) {
    g_instance = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
}

AltTabHook::~AltTabHook() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
    }
    if (g_instance == this) {
        g_instance = nullptr;
    }
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

bool AltTabHook::HandleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data) {
    // Track Shift purely from observed hook events, not GetAsyncKeyState
    // -- confirmed via docs that async key state can be stale inside a
    // low-level hook callback (this is explicitly documented for Alt;
    // tracking Shift the same way is the cheap, consistent choice).
    if (IsShiftKey(data.vkCode)) {
        shiftHeld_ = IsDown(wParam);
        return false;
    }

    // Alt-held combos arrive as WM_SYSKEYDOWN/WM_SYSKEYUP, not
    // WM_KEYDOWN/WM_KEYUP -- IsDown/IsUp above already account for that.
    // LLKHF_ALTDOWN, not GetAsyncKeyState, for the same staleness reason.
    const bool altHeld = (data.flags & LLKHF_ALTDOWN) != 0;

    if (data.vkCode == VK_TAB && altHeld && IsDown(wParam)) {
        if (!sessionActive_) {
            InjectHarmlessKeystroke();  // once per session -- see comment above
        }
        sessionActive_ = true;
        PostMessageW(messageWindow_, kHookMessage,
                     static_cast<WPARAM>(shiftHeld_ ? HookAction::CycleBackward : HookAction::CycleForward), 0);
        return true;
    }
    if (data.vkCode == VK_TAB && altHeld && IsUp(wParam)) {
        return true;  // swallow the matching up, down was swallowed above
    }

    if (IsAltKey(data.vkCode) && IsUp(wParam) && sessionActive_) {
        // Deliberately NOT swallowed (return false below, after posting)
        // -- see InjectHarmlessKeystroke's comment for why swallowing
        // Alt-up specifically must never happen.
        sessionActive_ = false;
        PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::Commit), 0);
        return false;
    }

    if (data.vkCode == VK_ESCAPE && IsDown(wParam) && sessionActive_) {
        // Ends the session outright (unlike Alt-up above, Escape is safe
        // to swallow -- it's not a modifier, so it carries none of the
        // stuck-state risk). If Alt is still physically held afterward
        // and the user presses Tab again, that correctly starts a fresh
        // session rather than silently doing nothing.
        sessionActive_ = false;
        PostMessageW(messageWindow_, kHookMessage, static_cast<WPARAM>(HookAction::Cancel), 0);
        return true;
    }

    return false;
}

void AltTabHook::HandleHookMessage(WPARAM wParam) {
    switch (static_cast<HookAction>(wParam)) {
        case HookAction::CycleForward:
            if (onCycle_) {
                onCycle_(/*backward=*/false);
            }
            break;
        case HookAction::CycleBackward:
            if (onCycle_) {
                onCycle_(/*backward=*/true);
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
    }
}

}  // namespace polish

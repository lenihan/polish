#pragma once

#include <windows.h>

#include <functional>

namespace polish {

// Intercepts Alt+Tab via a WH_KEYBOARD_LL hook so Windows' native
// switcher never appears, in favor of Polish's own (in progress -- see
// PLAN.md's Alt+Tab design notes). There is no documented API to filter
// native Alt+Tab's own contents, which is why this exists at all.
//
// Two things confirmed empirically before/while writing this for real
// (see PLAN.md): Alt-held combos arrive as WM_SYSKEYDOWN/WM_SYSKEYUP,
// not WM_KEYDOWN/WM_KEYUP; and swallowing Tab-down while leaving Alt's
// own down/up unswallowed can make target apps (Notepad, Explorer,
// others) see a "naked" Alt press and flash their menu bar. The fix is
// NOT to also swallow the matching Alt-up -- that was tried first and
// caused a worse, real bug: Windows' own internal modifier-state
// tracking never sees a swallowed key-up, so the whole session is left
// believing Alt is still held, breaking keyboard input system-wide even
// after this process exits. Instead, a harmless dummy keystroke (Ctrl
// tap) is injected via SendInput right after the first swallowed Tab,
// which breaks the "standalone Alt press" condition that triggers
// menu-mnemonic focus -- the real Alt-up is then always let through
// completely untouched. See AltTabHook.cpp's InjectHarmlessKeystroke.
//
// The hook proc itself only recognizes the relevant keystrokes and
// posts to messageWindow -- it deliberately never does heavier work
// (building the actual candidate list with its overlays, touching DWM,
// etc.) synchronously inside the callback. Windows silently unhooks a
// low-level hook that doesn't return promptly (LowLevelHooksTimeout,
// ~1000ms default on Win10 1709+), so the callback must stay trivial no
// matter what.
//
// One deliberate exception: hasEligibleCandidates (see constructor) IS
// called synchronously from inside the callback, on the first Tab of a
// session only. Without it, the hook always swallows Tab-while-Alt
// regardless of whether there's anywhere to switch to, and with 0 or 1
// non-minimized windows open that ate the keystroke into total
// silence -- confirmed, human-reported, as feeling broken rather than
// looking like "nothing to do here." EnumWindows plus cheap per-window
// style checks (what hasEligibleCandidates does) is a bounded,
// synchronous, no-UI operation, nowhere near the timeout risk that
// candidate popups/DWM calls would be -- it's the *rendering* work that
// stays deferred via the posted message below, not this.
class AltTabHook {
public:
    // hasEligibleCandidates(): called synchronously, only when a session
    //   isn't already active, to decide whether to swallow Tab-while-Alt
    //   at all. Returning false lets the keystroke fall through to
    //   native Alt+Tab untouched -- no onCycle/onCommit/onCancel fires
    //   for that keypress at all.
    // onCycle(backward): Tab (backward=false) or Shift+Tab (backward=true)
    //   pressed while Alt is held -- advance/reverse the highlight.
    // onCommit(): Alt released after at least one Tab was swallowed --
    //   caller should focus the currently-highlighted window.
    // onCancel(): Escape pressed while a session is active -- caller
    //   should dismiss without acting. Ends the session outright (unlike
    //   onCommit, which only fires on Alt-up): onCommit will NOT also
    //   fire afterward for the same Alt-hold, and if Alt is still
    //   physically held and Tab is pressed again, that correctly starts
    //   a fresh session.
    AltTabHook(HWND messageWindow, std::function<bool()> hasEligibleCandidates,
               std::function<void(bool backward)> onCycle, std::function<void()> onCommit,
               std::function<void()> onCancel);
    ~AltTabHook();

    AltTabHook(const AltTabHook&) = delete;
    AltTabHook& operator=(const AltTabHook&) = delete;

    // Routes the hook's private message. The hook callback runs on this
    // thread already (low-level hooks are called on the installing
    // thread), so this posts via PostMessage to messageWindow rather
    // than calling back directly -- keeps the hook callback itself down
    // to just recognizing keys and posting, per the timeout risk above.
    // Callers must route WM messages with message id == kHookMessage
    // here from their WindowProc.
    void HandleHookMessage(WPARAM wParam);

    // Whether SetWindowsHookExW actually succeeded -- callers should log
    // a warning if not (mirrors HotkeyManager::IsRegistered()'s pattern).
    bool IsInstalled() const { return hook_ != nullptr; }

    static constexpr UINT kHookMessage = WM_APP + 10;

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    bool HandleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data);

    HWND messageWindow_;
    std::function<bool()> hasEligibleCandidates_;
    std::function<void(bool backward)> onCycle_;
    std::function<void()> onCommit_;
    std::function<void()> onCancel_;
    HHOOK hook_ = nullptr;

    // True from the first swallowed Tab-while-Alt-held until the
    // matching Alt-up is swallowed (via commit or, if Escape cancelled
    // first, whenever Alt is eventually released) -- see class comment.
    bool sessionActive_ = false;
    bool shiftHeld_ = false;
};

}  // namespace polish

#pragma once

#include <windows.h>

#include <functional>

namespace polish {

// Intercepts Alt+Tab via a WH_KEYBOARD_LL hook so Windows' native
// switcher never appears, in favor of Polish's own (in progress -- see
// PLAN.md's Alt+Tab design notes). There is no documented API to filter
// native Alt+Tab's own contents, which is why this exists at all.
//
// Escape hatch: holding Ctrl along with Alt+Tab (Ctrl+Alt+Tab) bypasses
// Polish entirely for that gesture -- native Windows Alt+Tab handles it
// instead, untouched. A deliberate backup, not just an incidental side
// effect of some other check.
//
// While a session is active, a second, dynamically installed/uninstalled
// WH_MOUSE_LL hook watches for any mouse button press and immediately
// commits (not cancels -- see onCommit) the moment one happens, ending
// the session on whatever's currently highlighted. The click itself is
// deliberately left untouched (not swallowed) -- it reaches whatever's
// actually under the cursor completely normally, same as always (the dim
// overlays are WS_EX_TRANSPARENT on purpose, so nothing about them
// blocks it either way), so a single click can e.g. both minimize some
// window and land Alt+Tab on whatever was highlighted, which don't have
// to be the same window. Exists because leaving a session open through a
// click was confirmed as a real bug: closing, minimizing, or maximizing
// a window changes its shape or makes it stop existing while Polish's
// own session state still pointed at it, leaving the highlight/dim
// visuals stale. Installed only while a session is open (not for the
// whole app lifetime, unlike the keyboard hook) specifically to avoid
// paying for a global mouse hook's overhead (mouse-move volume is much
// higher than keyboard) outside the brief
// window where it's actually needed.
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
//
// A second exception, same category: isOwnUI (see SetIsOwnUI), if set, is
// also called synchronously -- from inside the mouse hook this time --
// before deciding whether a click commits the session. It exists because
// the mouse hook's default behavior (any button-down anywhere commits
// unconditionally, see HandleMouseEvent) would otherwise swallow a click
// meant for Polish's own list-panel UI as a generic commit trigger,
// instead of letting it reach that panel's own row/button hit-testing. A
// WindowFromPoint-plus-handle-comparison check is the same bounded,
// synchronous, no-UI shape as hasEligibleCandidates above.
//
// Arrow-key (Up/Down) navigation, once a session is already active, is
// recognized the same way Tab is -- swallowed and posted via onNavigate --
// but strictly gated on sessionActive_ already being true: unlike Tab,
// arrow keys are used constantly system-wide, so they must never be able
// to start a session and must be completely inert otherwise. The same
// physically-down debounce pattern used for tabPhysicallyDown_ applies
// per-key here too, so OS auto-repeat on a held arrow doesn't rapid-cycle
// through the list -- the identical bug shape already found and fixed
// once for Tab itself.
//
// Delete/-/+ (see SetOnRowAction), once a session is already active, are
// recognized and debounced the exact same way -- close/minimize-toggle/
// maximize-toggle the currently Tab-highlighted row without a mouse.
class AltTabHook {
public:
    // hasEligibleCandidates(): called synchronously, only when a session
    //   isn't already active, to decide whether to swallow Tab-while-Alt
    //   at all. Returning false lets the keystroke fall through to
    //   native Alt+Tab untouched -- no onCycle/onCommit/onCancel fires
    //   for that keypress at all.
    // onCycle(backward): Tab (backward=false) or Shift+Tab (backward=true)
    //   pressed while Alt is held -- advance/reverse the highlight.
    // onCommit(): Alt released after at least one Tab was swallowed, OR
    //   a mouse button was pressed during an active session -- caller
    //   should focus the currently-highlighted window either way.
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

    // Optional: called synchronously from the mouse hook (see class
    // comment) with the screen point of a button-down, to decide whether
    // that click is on Polish's own list-panel UI rather than some other
    // real window. When it returns true, the click is left completely
    // untouched (not treated as a generic commit trigger) so the panel's
    // own hit-testing can handle it. Unset (or returning false for a
    // given point) preserves today's behavior exactly.
    void SetIsOwnUI(std::function<bool(POINT screenPt)> isOwnUI) { isOwnUI_ = std::move(isOwnUI); }

    // Optional: Up/Down arrow-key navigation while a session is already
    // active (see class comment) -- downward=true for Down, false for Up.
    // Never fires unless sessionActive_ is already true.
    void SetOnNavigate(std::function<void(bool downward)> onNavigate) { onNavigate_ = std::move(onNavigate); }

    // Del/-/+ pressed while a session is already active -- keyboard
    // equivalents of clicking a row's close/minimize-toggle/maximize-
    // toggle action button (see AltTabListWindow), but with no keyboard
    // equivalent of "hover" these always mean whichever row is currently
    // Tab-highlighted; the callback itself is expected to resolve that.
    // Never fires unless sessionActive_ is already true, same gating as
    // arrow-key navigation.
    enum class RowAction { Close, MinimizeToggle, MaximizeToggle };
    void SetOnRowAction(std::function<void(RowAction action)> onRowAction) { onRowAction_ = std::move(onRowAction); }

    // Routes the hook's private message. The hook callback runs on this
    // thread already (low-level hooks are called on the installing
    // thread), so this posts via PostMessage to messageWindow rather
    // than calling back directly -- keeps the hook callback itself down
    // to just recognizing keys and posting, per the timeout risk above.
    // Callers must route WM messages with message id == kHookMessage
    // here from their WindowProc, passing both wParam and lParam through
    // unchanged (lParam carries the RowAction payload for that action;
    // every other message ignores it).
    void HandleHookMessage(WPARAM wParam, LPARAM lParam);

    // GetTickCount64() at the moment the first Tab-while-Alt of the
    // current/most recent session was detected in the hook -- a cheap,
    // no-I/O call (unlike LogDebug, which isn't done from inside the
    // hook itself). Callers can diff this against GetTickCount64() when
    // they actually start processing the resulting onCycle to see how
    // much of any visible delay is message-queue latency versus their
    // own rendering work -- added to chase a human-reported "flash of
    // the previous window" glitch with real evidence instead of guessing
    // again.
    ULONGLONG LastTabDetectedTick() const { return lastTabDetectedTick_; }

    // Whether SetWindowsHookExW actually succeeded -- callers should log
    // a warning if not (mirrors HotkeyManager::IsRegistered()'s pattern).
    bool IsInstalled() const { return hook_ != nullptr; }

    static constexpr UINT kHookMessage = WM_APP + 10;

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);
    bool HandleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data);
    bool HandleMouseEvent(WPARAM wParam, POINT screenPt);
    void InstallMouseHook();
    void UninstallMouseHook();

    HWND messageWindow_;
    std::function<bool()> hasEligibleCandidates_;
    std::function<void(bool backward)> onCycle_;
    std::function<void()> onCommit_;
    std::function<void()> onCancel_;
    std::function<bool(POINT screenPt)> isOwnUI_;
    std::function<void(bool downward)> onNavigate_;
    std::function<void(RowAction action)> onRowAction_;
    HHOOK hook_ = nullptr;
    HHOOK mouseHook_ = nullptr;

    // True from the first swallowed Tab-while-Alt-held until the
    // matching Alt-up is swallowed (via commit or, if Escape cancelled
    // first, whenever Alt is eventually released) -- see class comment.
    bool sessionActive_ = false;
    bool shiftHeld_ = false;
    bool ctrlHeld_ = false;

    // Tracked the same observed-from-hook-events way as shiftHeld_/
    // ctrlHeld_ -- true from a genuine Tab-down until its matching
    // Tab-up, so OS key-repeat (which resends Tab-down repeatedly while
    // it's held, with no repeat-count exposed to a low-level hook) can
    // be told apart from an actual fresh press. Holding Tab down
    // shouldn't rapidly cycle through windows any more than native
    // Alt+Tab does.
    bool tabPhysicallyDown_ = false;

    // Same debounce shape as tabPhysicallyDown_, one per arrow key (Up and
    // Down are independent keys, so a single shared flag can't tell them
    // apart if both happened to be down, however unlikely).
    bool upPhysicallyDown_ = false;
    bool downPhysicallyDown_ = false;

    // Same debounce shape again, one per row-action key -- holding
    // Minus/Plus down shouldn't rapidly toggle minimize/maximize via OS
    // key-repeat any more than holding Tab should rapidly cycle.
    bool deletePhysicallyDown_ = false;
    bool minusPhysicallyDown_ = false;
    bool plusPhysicallyDown_ = false;

    // True for the rest of the current Alt-hold once Ctrl+Alt+Tab (the
    // deliberate escape hatch to native Alt+Tab -- see HandleKeyEvent)
    // has handed off. While true, every subsequent Tab is also left
    // completely untouched, so native retains full control until Alt is
    // released -- otherwise releasing Ctrl mid-hold could make Polish
    // start intercepting midway through native's own switcher session.
    bool nativeHandoffActive_ = false;

    ULONGLONG lastTabDetectedTick_ = 0;
};

}  // namespace polish

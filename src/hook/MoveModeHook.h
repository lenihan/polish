#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>

namespace polish {

// The input side of easy move/resize mode: hold Win, and every window
// becomes draggable and resizable from anywhere inside it rather than
// from its title bar and its ~7px invisible border. See
// windowtracking/MoveSnap.h for the geometry and main.cpp for the
// visuals.
//
// A class of its own, with its own static g_instance (see the .cpp),
// rather than a third job on AltTabHook. That class keeps a single static
// pointer set by its constructor, so a second instance of it would
// silently steal every callback from the first -- a bug with no symptom
// at the point it is introduced. TaskbarHook is separate for exactly the
// same reason, and Windows chains the hooks fine.
//
// Shape of the gesture, and why it is this one:
//
//   - Win is the modifier because Alt+drag -- the other obvious choice,
//     and what AltDrag used -- is claimed inside real apps (Blender, CAD
//     tools, anything with a 3D viewport), and this has to work *on top
//     of* whatever app is under the cursor.
//   - The dim does not appear until the hold has lasted ~250ms, and any
//     other key during the hold hands the whole hold back to Windows
//     untouched (see nativeHandoffActive_). Without both of those, every
//     Win+L, Win+D, Win+Arrow and Win+number would flash the screen on
//     the way to doing something else entirely.
//   - The left button does everything, and where you press decides what
//     it does: an inch-wide band around the window's edge resizes,
//     everything inside it moves (MoveSnap.h's Grip). An earlier version
//     used the right button for resize, which is genuinely awkward on a
//     touchpad -- and a touchpad is where this feature gets used most.
//   - Space latches a keyboard session that survives Win-up, which is the
//     only way to offer arrow-key move/resize at all: Win+Arrow is
//     native Snap and is not available to take.
//
// Three rules this inherits from the hooks that came before it, each one
// paid for once already:
//
//   1. Never swallow WM_MOUSEMOVE. Returning non-zero for a move freezes
//      the cursor, because the same input processing the hook gates is
//      what moves the pointer. Swallowing mouse *buttons* is fine. (From
//      TaskbarHook.h.)
//   2. Never swallow the Win key-up. Windows' own modifier-state
//      tracking never sees a swallowed key-up, so the OS is left
//      believing the key is still held -- system-wide, surviving this
//      process exiting. AltTabHook.h has the full story; it was found
//      with Alt and the shape is identical here.
//   3. Because of (2), the Start menu has to be stopped some other way:
//      a standalone Win press-and-release opens it, and swallowing a
//      mouse button does not break "standalone". An invisible Ctrl tap
//      is injected instead, once per hold, the first time this hook does
//      anything the user can see -- see SuppressStartMenuForThisHold.
//
// The callback itself only recognizes input and PostMessageW's: Windows
// silently and undetectably unhooks a low-level hook that does not return
// promptly (LowLevelHooksTimeout, ~1000ms). Mouse-move volume during a
// drag makes this the hottest callback in the app, so drag positions are
// coalesced -- at most one Drag message is ever outstanding, and the
// handler reads the newest position rather than a queued stale one. A
// drag that generated 300 mouse-moves does not generate 300 SetWindowPos
// calls behind a message queue that is already running late.
//
// Two callbacks are called synchronously from inside the callback, the
// same deliberate exception AltTabHook makes for its own eligibility and
// own-UI checks: isEnabled and canGrab (see the constructor). Both have
// to be, because the answer decides whether the event in hand is
// swallowed, and a posted answer would arrive after it had already been
// dispatched.
//
// The mouse hook is installed only while a Win hold or a keyboard session
// is live, not for the app's lifetime -- the same lifecycle AltTabHook
// uses for its own, to avoid paying for a global mouse hook's overhead
// (mouse-move volume is much higher than keyboard's) at idle.
class MoveModeHook {
public:
    // A keyboard-session request. One packed value rather than a
    // callback per key: Move/Resize/Jump are the same request in three
    // flavours (plain Arrow, Shift+Arrow, Ctrl+Arrow) and twelve
    // callbacks would be noise.
    struct KeyCommand {
        enum class Kind {
            // Arrow: nudge by MoveSnap.h's kKeyboardStepPx, snapping.
            Move,
            // Shift+Arrow: grow or shrink the active grip's edges.
            Resize,
            // Ctrl+Arrow: jump flush to the next snap target that way.
            Jump,
            // Tab: change which corner Resize/Jump act on. dx/dy are 0.
            CycleGrip,
        };
        Kind kind = Kind::Move;
        // -1, 0 or +1. Screen axes: +x is right, +y is down.
        int dx = 0;
        int dy = 0;
    };

    // isEnabled(): whether the feature is switched on at all (the tray
    //   toggle). Called synchronously on a fresh Win-down; false leaves
    //   the entire hold completely untouched, so switching the feature
    //   off has to be indistinguishable from this hook not existing.
    // canGrab(screenPt): whether there is something at this point that
    //   Polish may actually move -- a real, non-elevated, candidate
    //   top-level window. Called synchronously on a button-down, and the
    //   answer decides whether that button is swallowed: returning false
    //   lets the click through to whatever is under it completely
    //   normally, which is what makes Win+click on the desktop, the
    //   taskbar, or an elevated window behave as it always did instead of
    //   vanishing into a mode that had nothing to offer. Must stay
    //   bounded and do no UI work -- WindowFromPoint plus cheap style
    //   checks is the intended shape (see windowtracking/WindowFilters.h).
    MoveModeHook(HWND messageWindow, std::function<bool()> isEnabled,
                 std::function<bool(POINT screenPt)> canGrab);
    ~MoveModeHook();

    MoveModeHook(const MoveModeHook&) = delete;
    MoveModeHook& operator=(const MoveModeHook&) = delete;

    // The Win key went down and the feature is on. The caller should
    // start its dim delay timer here, not dim immediately -- see the
    // class comment on why the hold has to stay invisible for a moment.
    void SetOnArm(std::function<void()> onArm) { onArm_ = std::move(onArm); }

    // The session is over and nothing is pending: undim, drop the
    // outline, forget the target. Fires for every ending that isn't a
    // cancel or a commit -- Win released with nothing done, or the hold
    // handed back to Windows because another key was pressed. The
    // caller's teardown must be idempotent; this can arrive for a hold
    // that never dimmed at all.
    void SetOnEnd(std::function<void()> onEnd) { onEnd_ = std::move(onEnd); }

    // The left button went down on something grabbable, at this screen
    // point. The caller should resolve the window, work out from the
    // point whether this is a move or a resize, sample its rect and
    // insets, and dim immediately if the delay timer hasn't fired yet.
    //
    // The hook deliberately does not classify the gesture: which zone a
    // point falls in needs the target's rect and DPI, which is the
    // caller's business, and the hook has no reason to learn it.
    void SetOnGrab(std::function<void(POINT screenPt)> onGrab) { onGrab_ = std::move(onGrab); }

    // The pointer moved, either during a drag or while the mode is merely
    // armed and hovering -- the caller distinguishes them by whether it
    // has a window in hand, and uses the hover case to show which window
    // a click would grab. Coalesced: this is the newest position at the
    // moment of delivery, not a queued older one.
    void SetOnDrag(std::function<void(POINT screenPt)> onDrag) { onDrag_ = std::move(onDrag); }

    // The drag's button came back up. The window keeps where it landed.
    // The session itself continues if Win is still held, so several
    // windows can be moved in one hold.
    void SetOnDrop(std::function<void()> onDrop) { onDrop_ = std::move(onDrop); }

    // Space was pressed during a hold: latch a keyboard session on the
    // window at this point (or, if there is none there, the caller's
    // choice of the foreground window). Outlives the Win-up that follows.
    void SetOnKeyboardLatch(std::function<void(POINT screenPt)> onLatch) { onKeyboardLatch_ = std::move(onLatch); }

    // A keyboard-session key. Never fires unless a keyboard session is
    // already latched -- arrows, Tab and Enter are used constantly
    // system-wide and must be completely inert the rest of the time, the
    // same gating AltTabHook puts on its navigation keys.
    void SetOnKeyCommand(std::function<void(KeyCommand command)> onKeyCommand) {
        onKeyCommand_ = std::move(onKeyCommand);
    }

    // Escape: put the window back exactly where it started and end.
    void SetOnCancel(std::function<void()> onCancel) { onCancel_ = std::move(onCancel); }

    // Enter, or any mouse button during a keyboard session: keep where
    // the window is and end. A click is not swallowed -- it reaches
    // whatever is under the cursor normally, the same bargain
    // AltTabHook's click-commits makes.
    void SetOnCommit(std::function<void()> onCommit) { onCommit_ = std::move(onCommit); }

    // Injects the invisible Ctrl tap that stops the Start menu opening
    // on the Win-up at the end of this hold, if it hasn't been injected
    // already. Idempotent per hold.
    //
    // Public because the hook cannot tell when the mode first becomes
    // *visible*: the dim is on a timer the caller owns, and a hold long
    // enough to dim must not open Start even if the user then does
    // nothing else. The hook calls this itself for the other case, a
    // swallowed button. Safe to call from the message thread; it is a
    // single SendInput of two events.
    void SuppressStartMenuForThisHold();

    // Routes this hook's private message. The hook callback runs on the
    // installing thread -- the same thread as the message loop -- so this
    // posts rather than calling back directly, keeping the callback down
    // to recognize-and-post per the timeout risk above. Callers must
    // route messages with id == kMoveModeMessage here from their
    // WindowProc, passing wParam and lParam through unchanged.
    void HandleHookMessage(WPARAM wParam, LPARAM lParam);

    // Whether SetWindowsHookExW actually succeeded -- callers should log
    // a warning if not, same as AltTabHook::IsInstalled.
    bool IsInstalled() const { return hook_ != nullptr; }

    // Whether a drag or a keyboard session is currently moving a window.
    // Read by the caller to suppress restore-position sync while Polish
    // is the one doing the moving: a Polish-driven drag produces an
    // EVENT_OBJECT_LOCATIONCHANGE flood with no
    // EVENT_SYSTEM_MOVESIZESTART/END around it, so the settle debounce
    // would otherwise record mid-drag rects as the restore position.
    bool IsMovingWindow() const { return dragging_ || keyboardSession_; }

    // WM_APP+1 is TrayIcon's, +10 AltTabHook's, +20 the deferred
    // close-group cleanup in main.cpp.
    static constexpr UINT kMoveModeMessage = WM_APP + 30;

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);
    bool HandleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data);
    bool HandleMouseEvent(WPARAM wParam, POINT screenPt);
    void InstallMouseHook();
    void UninstallMouseHook();
    // Every path that ends a session goes through here, so the mouse-hook
    // uninstall and the per-hold flag resets cannot drift apart between
    // them -- the same single-teardown-path discipline AltTabHook::
    // EndSession keeps. `action` is the message posted to say so.
    void EndSession(WPARAM action);
    void Post(WPARAM action, LPARAM payload = 0);

    HWND messageWindow_;
    std::function<bool()> isEnabled_;
    std::function<bool(POINT screenPt)> canGrab_;
    std::function<void()> onArm_;
    std::function<void()> onEnd_;
    std::function<void(POINT)> onGrab_;
    std::function<void(POINT)> onDrag_;
    std::function<void()> onDrop_;
    std::function<void(POINT)> onKeyboardLatch_;
    std::function<void(KeyCommand)> onKeyCommand_;
    std::function<void()> onCancel_;
    std::function<void()> onCommit_;
    HHOOK hook_ = nullptr;
    HHOOK mouseHook_ = nullptr;

    // None of this is atomic, deliberately: a low-level hook is called
    // on the thread that installed it, which here is the same thread that
    // runs the message loop and therefore HandleHookMessage. There is no
    // second thread to race with.

    // True from a Win-down the feature accepted until the matching
    // Win-up. "Armed", not "active": whether anything is on screen yet is
    // the caller's business, decided by its dim timer.
    bool armed_ = false;
    // True while Win is physically down, tracked purely from observed
    // hook events rather than GetAsyncKeyState -- async key state is
    // documented as unreliable inside a low-level hook callback, and
    // unlike Alt (LLKHF_ALTDOWN) there is no event flag for Win.
    bool winHeld_ = false;
    // The rest of this Win hold belongs to Windows. Latched when any key
    // other than the ones this mode claims arrives during a hold, so
    // Win+L, Win+D, Win+Arrow and Win+<digit> behave exactly as they
    // always did and never flash the dim. Reset on Win-up. Same idea as
    // AltTabHook's nativeHandoffActive_.
    bool nativeHandoffActive_ = false;
    // The left button is down and a window is following the pointer.
    // Survives Win-up on purpose: the button, not the modifier, owns a
    // drag in progress, and yanking the window to a stop because a finger
    // left the Win key mid-throw is not what anyone means.
    bool dragging_ = false;
    // A latched keyboard session, which outlives the hold that started
    // it and is ended only by Enter, Escape, a click, or the caller
    // noticing its target is gone.
    bool keyboardSession_ = false;
    // See SuppressStartMenuForThisHold. Reset on Win-up.
    bool startMenuSuppressedThisHold_ = false;
    // Tracked the same observed-from-hook-events way as winHeld_, and
    // for the same staleness reason. These pick which flavour an arrow
    // key means in a keyboard session: plain moves, Shift resizes, Ctrl
    // jumps to the next snap target.
    bool shiftHeld_ = false;
    bool ctrlHeld_ = false;

    // The newest pointer position the hook has seen, and whether a Drag
    // message for it is already in the queue. Together these are the
    // coalescing described in the class comment: the position is
    // overwritten freely while a single message waits its turn.
    POINT lastMousePoint_{};
    bool dragPostPending_ = false;
    // Where the grab started, read by the Grab handler. Not coalesced --
    // a grab cannot be followed by another before its drop.
    POINT grabPoint_{};

    // Per-key auto-repeat debounce, same shape as AltTabHook's
    // tabPhysicallyDown_: a low-level hook sees no repeat count, so a
    // held key resends down events indefinitely and would otherwise
    // scroll a window off the screen from one press. Escape and Enter
    // are in here for the key-up pairing rather than for repeat
    // suppression -- see HandleKeyEvent.
    //
    // The slot list and the array's size are deliberately one
    // declaration, with kDebouncedKeyCount as the enum's own last
    // member. They were briefly two, in two files, and went out of sync
    // the moment a slot was added -- writing two keys past the end of
    // the array, which a plain C array does not notice and the build did
    // not either. DebounceSlotFor is a member for the same reason: it
    // has to see these names.
    enum DebouncedKey { kLeft, kRight, kUp, kDown, kSpaceKey, kTabKey, kEscapeKey, kEnterKey, kDebouncedKeyCount };
    static int DebounceSlotFor(DWORD vk);
    bool keysPhysicallyDown_[kDebouncedKeyCount] = {};
};

}  // namespace polish

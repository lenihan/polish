#include "hook/MoveModeHook.h"

namespace polish {

namespace {

// Only one instance is ever installed for the app's lifetime, and
// low-level hooks have no user-data parameter, so a plain static pointer
// is the simplest correct option -- the same pattern AltTabHook and
// TaskbarHook each keep for themselves. Separate statics is exactly why
// those two can coexist and why this is a third class rather than a third
// job on one of them.
MoveModeHook* g_instance = nullptr;

enum class HookAction : WPARAM {
    Arm,
    End,
    GrabMove,
    GrabResize,
    Drag,
    Drop,
    KeyboardLatch,
    // Carries a packed KeyCommand in lParam -- see PackKeyCommand.
    Key,
    Cancel,
    Commit,
};

bool IsDown(WPARAM wParam) { return wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN; }
bool IsShiftKey(DWORD vk) { return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT; }
bool IsCtrlKey(DWORD vk) { return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }
bool IsWinKey(DWORD vk) { return vk == VK_LWIN || vk == VK_RWIN; }

bool IsButtonDown(WPARAM wParam) {
    return wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN ||
           wParam == WM_XBUTTONDOWN;
}

bool IsButtonUp(WPARAM wParam) {
    return wParam == WM_LBUTTONUP || wParam == WM_RBUTTONUP || wParam == WM_MBUTTONUP ||
           wParam == WM_XBUTTONUP;
}

// A third copy of this, after AltTabHook.cpp's InjectHarmlessKeystroke
// and main.cpp's InjectHarmlessCtrlKeystroke. Deliberate, and the same
// call those two make for the same reason: two SendInput events, no
// state, no dependencies, and each copy sits next to the specific bug it
// prevents. Sharing it would mean a header dependency between three
// unrelated layers to save four lines.
//
// Here the bug it prevents is the Start menu. A standalone Win press and
// release opens it, and the release can never be swallowed (see rule 2 in
// MoveModeHook.h), so the sequence has to stop being standalone instead:
// an invisible Ctrl tap in the middle of the hold does that. Injected
// events come back through this same hook carrying LLKHF_INJECTED, which
// HandleKeyEvent checks for -- without that check this tap would look
// like the user pressing another key and would hand the hold straight
// back to Windows.
void InjectHarmlessKeystroke() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_CONTROL;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

LPARAM PackKeyCommand(const MoveModeHook::KeyCommand& command) {
    // dx/dy are -1..+1, biased by one so each fits a byte unsigned.
    return (static_cast<LPARAM>(command.kind) << 16) | (static_cast<LPARAM>(command.dx + 1) << 8) |
           static_cast<LPARAM>(command.dy + 1);
}

MoveModeHook::KeyCommand UnpackKeyCommand(LPARAM packed) {
    MoveModeHook::KeyCommand command;
    command.kind = static_cast<MoveModeHook::KeyCommand::Kind>((packed >> 16) & 0xFF);
    command.dx = static_cast<int>((packed >> 8) & 0xFF) - 1;
    command.dy = static_cast<int>(packed & 0xFF) - 1;
    return command;
}

}  // namespace

// Which debounce slot a key uses, or -1 for a key that has none. A
// member rather than a file-local helper so it shares one declaration
// with the slots and the array they index -- see DebouncedKey.
int MoveModeHook::DebounceSlotFor(DWORD vk) {
    switch (vk) {
        case VK_LEFT:
            return kLeft;
        case VK_RIGHT:
            return kRight;
        case VK_UP:
            return kUp;
        case VK_DOWN:
            return kDown;
        case VK_SPACE:
            return kSpaceKey;
        case VK_TAB:
            return kTabKey;
        case VK_ESCAPE:
            return kEscapeKey;
        case VK_RETURN:
            return kEnterKey;
        default:
            return -1;
    }
}

MoveModeHook::MoveModeHook(HWND messageWindow, std::function<bool()> isEnabled,
                           std::function<bool(POINT)> canGrab)
    : messageWindow_(messageWindow), isEnabled_(std::move(isEnabled)), canGrab_(std::move(canGrab)) {
    g_instance = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
}

MoveModeHook::~MoveModeHook() {
    UninstallMouseHook();
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
    }
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

void MoveModeHook::InstallMouseHook() {
    if (mouseHook_ == nullptr) {
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
    }
}

void MoveModeHook::UninstallMouseHook() {
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

void MoveModeHook::Post(WPARAM action, LPARAM payload) {
    PostMessageW(messageWindow_, kMoveModeMessage, action, payload);
}

void MoveModeHook::EndSession(WPARAM action) {
    armed_ = false;
    dragging_ = false;
    keyboardSession_ = false;
    dragPostPending_ = false;
    // nativeHandoffActive_ is deliberately NOT reset here: it has to
    // outlive the session it ended and keep the rest of this Win hold
    // hands-off, or the very next click in the same hold would be
    // swallowed again. Win-down and Win-up are the only things that
    // clear it.
    UninstallMouseHook();
    Post(action);
}

void MoveModeHook::SuppressStartMenuForThisHold() {
    if (startMenuSuppressedThisHold_) {
        return;
    }
    startMenuSuppressedThisHold_ = true;
    InjectHarmlessKeystroke();
}

LRESULT CALLBACK MoveModeHook::LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance != nullptr) {
        const auto* data = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (g_instance->HandleKeyEvent(wParam, *data)) {
            return 1;  // swallow: block this event from reaching its target window
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK MoveModeHook::LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance != nullptr) {
        const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (g_instance->HandleMouseEvent(wParam, data->pt)) {
            return 1;  // swallow: this button belongs to the mode, not to the app under it
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool MoveModeHook::HandleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data) {
    const DWORD vk = data.vkCode;
    const bool down = IsDown(wParam);

    // Modifier state is tracked purely from observed hook events, never
    // GetAsyncKeyState, which is documented as unreliable inside a
    // low-level hook callback. Never swallowed either way.
    if (IsShiftKey(vk)) {
        shiftHeld_ = down;
        return false;
    }
    if (IsCtrlKey(vk)) {
        ctrlHeld_ = down;
        return false;
    }

    if (IsWinKey(vk)) {
        if (down) {
            if (winHeld_) {
                // OS auto-repeat of the held Win key, and it has to be
                // swallowed once this mode has committed to the hold.
                //
                // The reason is not obvious and cost a user-reported bug:
                // every repeat key-down re-arms the shell's
                // "standalone Win press" condition, which throws away the
                // Ctrl tap SuppressStartMenuForThisHold already injected.
                // So after a real hold -- a physical key repeating at
                // ~30/s for the whole drag -- the Win-up at the end still
                // opened the Start menu, on top of the window just moved.
                // Measured against the bare OS with Polish stopped:
                // Win-down, Ctrl tap, Win-up leaves Start shut; the same
                // sequence with a dozen repeat downs inserted before the
                // up opens it every time.
                //
                // Synthetic input never showed this, because keybd_event
                // sends one down and no repeats. That is also why the
                // repeats are swallowed rather than answered with another
                // Ctrl tap per repeat: injecting ~30 keystrokes a second
                // into whatever app is under the cursor to work around a
                // keystroke we are already injecting is the worse trade.
                //
                // Safe to swallow, unlike the key-up: the OS took the key
                // as held from the first down, which is passed through
                // untouched, and nothing meaningful counts Win repeats.
                // Gated on having actually suppressed Start, so a hold
                // handed back to Windows (or one that has not done
                // anything yet) still delivers every repeat.
                return startMenuSuppressedThisHold_ && !nativeHandoffActive_;
            }
            winHeld_ = true;
            nativeHandoffActive_ = false;
            startMenuSuppressedThisHold_ = false;
            // A hold is pointless while a keyboard session already owns a
            // window -- the session has its own keys and does not need
            // the modifier.
            if (!keyboardSession_ && isEnabled_ && isEnabled_()) {
                armed_ = true;
                // Installed now rather than when the dim appears, so the
                // very first click of a fast hold-and-drag is not missed.
                InstallMouseHook();
                Post(static_cast<WPARAM>(HookAction::Arm));
            }
        } else {
            winHeld_ = false;
            nativeHandoffActive_ = false;
            startMenuSuppressedThisHold_ = false;
            if (armed_) {
                armed_ = false;
                // A drag or a keyboard session outlives the hold that
                // started it; everything else ends with it.
                if (!dragging_ && !keyboardSession_) {
                    EndSession(static_cast<WPARAM>(HookAction::End));
                }
            }
        }
        // Rule 2, the expensive one: never swallow a modifier's key-up.
        return false;
    }

    // Every debounced key's up-event is handled here, unconditionally and
    // before anything else can branch on the current state.
    //
    // This is load-bearing, and it was found the hard way: Space's down
    // is handled on the "armed" path and sets its flag, but by the time
    // its up arrives a keyboard session is latched, so the up took the
    // session branch instead and the flag was never cleared. The second
    // Win+Space of the session did nothing at all, and would have done
    // nothing for the rest of the process's life. Exactly the bug
    // AltTabHook's unconditional Tab-up reset exists to prevent --
    // a debounce flag must never be able to stick.
    //
    // The up is swallowed if and only if its down was, so the pair always
    // reaches the target window together or not at all.
    if (const int slot = DebounceSlotFor(vk); slot >= 0 && !down) {
        const bool downWasSwallowed = keysPhysicallyDown_[slot];
        keysPhysicallyDown_[slot] = false;
        return downWasSwallowed;
    }

    if (keyboardSession_) {
        switch (vk) {
            // Escape and Enter each flag themselves before ending the
            // session, purely so the generic up-handler above swallows
            // their key-up too. Without that the session is already gone
            // by the time the up arrives, so it would reach the window
            // on its own with no matching down -- inert in most apps,
            // but an unpaired keystroke is not something to ship on
            // purpose.
            case VK_ESCAPE:
                keysPhysicallyDown_[kEscapeKey] = true;
                EndSession(static_cast<WPARAM>(HookAction::Cancel));
                return true;
            case VK_RETURN:
                keysPhysicallyDown_[kEnterKey] = true;
                EndSession(static_cast<WPARAM>(HookAction::Commit));
                return true;
            case VK_TAB:
                if (keysPhysicallyDown_[kTabKey]) {
                    return true;  // repeat: swallowed, but does nothing
                }
                keysPhysicallyDown_[kTabKey] = true;
                Post(static_cast<WPARAM>(HookAction::Key),
                     PackKeyCommand({KeyCommand::Kind::CycleGrip, 0, 0}));
                return true;
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN: {
                const DebouncedKey slot = vk == VK_LEFT    ? kLeft
                                          : vk == VK_RIGHT ? kRight
                                          : vk == VK_UP    ? kUp
                                                           : kDown;
                // Arrow repeat is swallowed but inert: a held arrow would
                // otherwise fling a window across the desktop from one
                // press, the same bug shape AltTabHook found with Tab.
                if (keysPhysicallyDown_[slot]) {
                    return true;
                }
                keysPhysicallyDown_[slot] = true;
                KeyCommand command;
                command.kind = shiftHeld_  ? KeyCommand::Kind::Resize
                               : ctrlHeld_ ? KeyCommand::Kind::Jump
                                           : KeyCommand::Kind::Move;
                command.dx = vk == VK_LEFT ? -1 : vk == VK_RIGHT ? 1 : 0;
                command.dy = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : 0;
                Post(static_cast<WPARAM>(HookAction::Key), PackKeyCommand(command));
                return true;
            }
            default:
                // Everything else is left completely alone, so a session
                // never makes the keyboard feel broken.
                return false;
        }
    }

    if (!armed_ || nativeHandoffActive_) {
        return false;
    }

    if (vk == VK_SPACE) {
        if (keysPhysicallyDown_[kSpaceKey]) {
            return true;
        }
        keysPhysicallyDown_[kSpaceKey] = true;
        keyboardSession_ = true;
        // GetCursorPos rather than lastMousePoint_: the mouse hook only
        // learns a position from a move, and a hold that started without
        // the pointer being touched has not seen one. A bounded, no-UI
        // call, same category as the synchronous callbacks.
        GetCursorPos(&grabPoint_);
        SuppressStartMenuForThisHold();
        Post(static_cast<WPARAM>(HookAction::KeyboardLatch));
        return true;
    }

    if (down && (data.flags & LLKHF_INJECTED) == 0) {
        // Any other real key during a hold means the user is reaching for
        // something of Windows' own -- Win+L, Win+D, Win+Arrow,
        // Win+<digit>. Hand the whole rest of the hold back untouched and
        // get off the screen. The injected-event check matters: our own
        // Ctrl tap (see InjectHarmlessKeystroke) comes back through here
        // and would otherwise trip this on ourselves.
        nativeHandoffActive_ = true;
        EndSession(static_cast<WPARAM>(HookAction::End));
    }
    return false;
}

bool MoveModeHook::HandleMouseEvent(WPARAM wParam, POINT screenPt) {
    lastMousePoint_ = screenPt;

    if (wParam == WM_MOUSEMOVE) {
        // Rule 1: never swallow a move -- returning non-zero here freezes
        // the cursor, because the input processing this hook gates is what
        // moves the pointer.
        //
        // At most one Drag message is ever outstanding. The position above
        // is overwritten freely while it waits, so the handler always acts
        // on where the pointer is now rather than working through a queue
        // of stale positions it can never catch up with.
        //
        // Posted while merely armed as well as while dragging, because
        // the mode has something to say about a pointer that is only
        // hovering: which window would be grabbed. The caller tells the
        // two apart by whether it has a window in hand.
        if ((dragging_ || armed_) && !dragPostPending_) {
            dragPostPending_ = true;
            Post(static_cast<WPARAM>(HookAction::Drag));
        }
        return false;
    }

    if (keyboardSession_) {
        if (IsButtonDown(wParam)) {
            // Commits rather than cancels, and deliberately not swallowed
            // -- the click reaches whatever is under the cursor completely
            // normally while the session ends on the window it was
            // driving. The same bargain AltTabHook's click-commit makes,
            // for the same reason: leaving a session open through a click
            // leaves its visuals pointing at a window the click may have
            // just closed, minimized or reshaped.
            EndSession(static_cast<WPARAM>(HookAction::Commit));
        }
        return false;
    }

    if (dragging_) {
        const bool matchingUp =
            dragGrab_ == Grab::Move ? wParam == WM_LBUTTONUP : wParam == WM_RBUTTONUP;
        if (matchingUp) {
            dragging_ = false;
            dragPostPending_ = false;
            Post(static_cast<WPARAM>(HookAction::Drop));
            if (!winHeld_) {
                // The hold ended mid-drag and the drag was what kept the
                // session alive.
                EndSession(static_cast<WPARAM>(HookAction::End));
            }
            return true;  // swallow, pairing with the swallowed button-down
        }
        // Every other button event during a drag is swallowed too: a
        // stray right-click mid-move would otherwise open a context menu
        // on top of the window being dragged.
        return IsButtonDown(wParam) || IsButtonUp(wParam);
    }

    if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN) {
        if (!canGrab_ || !canGrab_(screenPt)) {
            // Nothing here Polish may move -- the desktop, the taskbar, an
            // elevated window. Leave the click completely alone so
            // Win+click behaves as it always did rather than disappearing
            // into a mode with nothing to offer. Called synchronously
            // because the answer decides whether to swallow; see the
            // class comment.
            return false;
        }
        dragging_ = true;
        dragGrab_ = wParam == WM_LBUTTONDOWN ? Grab::Move : Grab::Resize;
        grabPoint_ = screenPt;
        SuppressStartMenuForThisHold();
        Post(static_cast<WPARAM>(dragGrab_ == Grab::Move ? HookAction::GrabMove : HookAction::GrabResize));
        return true;  // swallow: this button belongs to the mode
    }
    return false;
}

void MoveModeHook::HandleHookMessage(WPARAM wParam, LPARAM lParam) {
    switch (static_cast<HookAction>(wParam)) {
        case HookAction::Arm:
            if (onArm_) {
                onArm_();
            }
            break;
        case HookAction::End:
            if (onEnd_) {
                onEnd_();
            }
            break;
        case HookAction::GrabMove:
            if (onGrab_) {
                onGrab_(Grab::Move, grabPoint_);
            }
            break;
        case HookAction::GrabResize:
            if (onGrab_) {
                onGrab_(Grab::Resize, grabPoint_);
            }
            break;
        case HookAction::Drag:
            // Cleared before the callback, so a move arriving while the
            // caller is still working can queue the next one.
            dragPostPending_ = false;
            if (onDrag_) {
                onDrag_(lastMousePoint_);
            }
            break;
        case HookAction::Drop:
            if (onDrop_) {
                onDrop_();
            }
            break;
        case HookAction::KeyboardLatch:
            if (onKeyboardLatch_) {
                onKeyboardLatch_(grabPoint_);
            }
            break;
        case HookAction::Key:
            if (onKeyCommand_) {
                onKeyCommand_(UnpackKeyCommand(lParam));
            }
            break;
        case HookAction::Cancel:
            if (onCancel_) {
                onCancel_();
            }
            break;
        case HookAction::Commit:
            if (onCommit_) {
                onCommit_();
            }
            break;
    }
}

}  // namespace polish

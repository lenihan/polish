#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace polish {

// The app-lifetime WH_MOUSE_LL hook behind Polish's taskbar behavior:
// which app button the pointer is over, whether a left-click on one
// should cycle that app's windows instead of doing what Windows would,
// and -- the part that makes the whole thing usable -- when the shield
// must get out of the way so an event reaches the real taskbar.
//
// A class of its own rather than a third job on AltTabHook, which already
// owns a keyboard hook and a temporary mouse one. That class keeps a
// single static g_instance set by its constructor, so a second instance
// of it would silently steal every callback from the first -- a bug with
// no symptom at the point it is introduced. This keeps its own static
// pointer, and the two hooks coexist fine (Windows chains them).
//
// Why a low-level hook, when the shield already sits over the strip and
// receives the same pointer: the shield gets one hit-test verdict per
// message and cannot tell a hover from a click. This can. It sees the
// message type before the event is dispatched to anyone, which is the
// only moment at which "this press is the taskbar's, let it through" can
// still be acted on.
//
// The callback obeys the same rule as AltTabHook's: recognize, then
// PostMessageW, and nothing else. Windows silently and undetectably
// unhooks a low-level hook that does not return promptly
// (LowLevelHooksTimeout, ~1000ms), and mouse-move volume makes this the
// hottest callback in the app. So the hook owns a plain copy of the
// button rects rather than reading the shared snapshot behind its lock,
// and resolving an app id to its windows -- far too slow here -- happens
// on the receiving end of the posted message.
//
// The one deliberate exception is setPassThrough (see the constructor),
// which is called synchronously. It has to be: posting it would land
// after the event it was meant to let through had already been
// dispatched. It is a single SetWindowLongPtrW per shield window, the
// same bounded, no-UI shape as AltTabHook's own synchronous callbacks.
//
// Two things it must never do, both measured:
//
//   - Never swallow WM_MOUSEMOVE. Returning non-zero for a move freezes
//     the cursor, because the same input processing the hook gates is
//     what moves the pointer. Swallowing mouse *buttons* is fine.
//   - Never claim a gesture while Ctrl is held. Ctrl is the escape hatch:
//     with it down the taskbar behaves exactly as it would without Polish
//     installed -- flyout included, which is why Ctrl turns pass-through
//     on rather than merely suppressing Polish's own handling.
class TaskbarHook {
public:
    // One app button, reduced to what the hook thread actually needs. No
    // app id and no window list: the hook reports an index into the
    // vector it was last given, and the caller -- which built that vector
    // -- resolves it back. Keeps COM, strings and allocation out of the
    // callback entirely.
    struct Target {
        // Screen rect in physical pixels, matching MSLLHOOKSTRUCT::pt so
        // the hit-test compares like with like. See TaskbarButton::rect
        // for why that holds only while the process stays
        // Per-Monitor-V2 aware.
        RECT rect{};
        // Whether a left-click here should be swallowed and turned into
        // a cycle -- true only for an app with 2+ windows open. With 0 or
        // 1, the click is passed to the taskbar so Windows can launch or
        // activate as it always has: there is nothing to cycle, and
        // eating the click would make the taskbar feel broken.
        bool cyclesOnClick = false;
    };

    // onHoverChanged(generation, index): the pointer moved onto a
    //   different button, or off the strip entirely (index -1). Fired on
    //   the change only, not per mouse-move, and fired regardless of Ctrl
    //   -- the caller needs to know the pointer is on the strip either
    //   way, and decides for itself what to show. No dwell is applied
    //   here: the hook reports what it sees.
    // onCycleClick(generation, index): a left-click was swallowed on a
    //   button whose Target::cyclesOnClick is set. Fired on the press;
    //   the matching release is swallowed too but reported to nobody.
    // setPassThrough(on): called synchronously, from inside the hook
    //   callback, to hand the shield over to the taskbar for this event
    //   or take it back. See the class comment for why this one cannot be
    //   posted like the others.
    //
    // The first two carry the generation of the target list the hit-test
    // was made against, because they arrive as posted messages: the
    // targets can be replaced in between, which would otherwise silently
    // act on whichever app has since taken that index.
    TaskbarHook(HWND messageWindow, std::function<void(uint64_t generation, int index)> onHoverChanged,
                std::function<void(uint64_t generation, int index)> onCycleClick,
                std::function<void(bool on)> setPassThrough);
    ~TaskbarHook();

    TaskbarHook(const TaskbarHook&) = delete;
    TaskbarHook& operator=(const TaskbarHook&) = delete;

    // Replaces what the hook hit-tests against, and stamps it with
    // `generation`. Call on the thread that installed the hook (the hook
    // callback runs there too, so the copy needs no lock -- see the .cpp).
    // An empty list makes the hook inert without uninstalling it.
    void SetTargets(uint64_t generation, std::vector<Target> targets);

    // Whether the shield should currently be letting events through:
    // Ctrl held, or a mouse button down that Polish has not claimed.
    //
    // Public because the hook alone cannot keep it current. It only ever
    // sees mouse events, so a Ctrl press or a button release with the
    // pointer held perfectly still never reaches it, and the shield would
    // stay in whatever state the last event left it. The caller polls
    // this on a short timer while the pointer is on the strip -- see
    // main.cpp's taskbar hover timer -- which is also what takes the
    // shield back after a click the taskbar was given.
    bool PassThroughWanted() const;

    // Whether SetWindowsHookExW succeeded. Callers should log a warning
    // if not, the same as AltTabHook::IsInstalled.
    bool IsInstalled() const { return hook_ != nullptr; }

    // Reinstalls the hook if it looks dead, and returns whether it did.
    // Meant for a low-frequency timer.
    //
    // A watchdog is not paranoia here: Windows removes a low-level hook
    // that overruns LowLevelHooksTimeout without telling the process, and
    // there is no API to ask whether a hook is still live. So "dead" is
    // inferred -- the system has seen input more recently than this hook
    // has seen an event. That also fires on someone typing without
    // touching the mouse, which is a false positive, and deliberately
    // tolerated: reinstalling costs one unhook and one hook and changes
    // nothing that is working.
    bool EnsureInstalled();

    // Routes the hook's private messages. Callers must forward both of
    // the ids below from their WindowProc with wParam and lParam
    // unchanged.
    void HandleHookMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // wParam is the target-list generation; lParam is the button index,
    // or -1 for kHoverMessage when the pointer left the strip.
    static constexpr UINT kHoverMessage = WM_APP + 11;
    static constexpr UINT kCycleClickMessage = WM_APP + 12;

    // A press the shield swallowed on the taskbar's behalf, to be sent
    // again now that the shield is open. wParam is the original mouse
    // message, lParam its MSLLHOOKSTRUCT::mouseData.
    //
    // Posted rather than sent from inside the hook, which is the whole
    // point of it: a replay issued from the callback lands too early and
    // is routed as though the shield were still closed. Measured three
    // ways -- in-hook replay, nothing; the same press 300ms later, the
    // jumplist opens; and a watch across a real press showing the shield
    // going pass-through 4ms in, so the style change itself was never in
    // question. See ReplayButtonDown.
    static constexpr UINT kReplayPressMessage = WM_APP + 13;

private:
    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);
    // Returns true to swallow the event. `mouseData` is
    // MSLLHOOKSTRUCT::mouseData, carried only so an X-button press can be
    // replayed as the same X button.
    bool HandleMouseEvent(WPARAM message, POINT screenPt, DWORD mouseData);
    bool Install();
    void Uninstall();
    int HitTest(POINT screenPt) const;
    void ApplyPassThrough(bool on);

    HWND messageWindow_;
    std::function<void(uint64_t generation, int index)> onHoverChanged_;
    std::function<void(uint64_t generation, int index)> onCycleClick_;
    std::function<void(bool on)> setPassThrough_;
    HHOOK hook_ = nullptr;

    std::vector<Target> targets_;
    uint64_t generation_ = 0;

    // Which button the pointer was last over, so a hover is reported once
    // on the change rather than on every move. -1 is off the strip.
    int hoveredIndex_ = -1;

    // A press swallowed for the taskbar, between the hook posting it and
    // the message loop sending it again. WM_NULL when there is none.
    //
    // It exists because the two can be raced by the user's own release: a
    // fast click can put the real button-up through before the replayed
    // press has been sent, which would leave the taskbar holding a press
    // that never ends. When that happens the release is swallowed too and
    // the replay becomes a whole click, at the cost of that one gesture
    // not being a drag -- which it was not, since it was over in less
    // than a message-loop turn.
    WPARAM pendingReplayMessage_ = WM_NULL;
    DWORD pendingReplayMouseData_ = 0;
    bool pendingReplayNeedsRelease_ = false;

    // True between a swallowed left-button-down and its matching up. The
    // up must be swallowed too: a button-up with no matching down still
    // reaches the taskbar's XAML pointer handling, which is enough for it
    // to treat the gesture as a click and activate the app itself --
    // defeating the point of eating the down. Tracked rather than
    // re-hit-tested on the up so a press that starts on a button and ends
    // somewhere else is still balanced.
    bool swallowedLeftDown_ = false;

    // GetTickCount64() at the last event this hook saw, for
    // EnsureInstalled's liveness inference.
    ULONGLONG lastEventTick_ = 0;
};

}  // namespace polish

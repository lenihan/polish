# Handoff — taskbar work

Scratch file for resuming after a `/clear`. Not part of the project; delete it
when the taskbar work is done. Paste the block below as the first message.

---

## The prompt

> Continue the Polish taskbar work. Read `HANDOFF.md` in the repo root first,
> then `docs/LIMITATIONS.md` #22 and #23 and the `Taskbar:` entries in
> `PLAN.md`.
>
> The shield, the hook, the hover panel and click-to-cycle are all built and
> verified live. What is left is in "What's next" below.
>
> Do not re-investigate how to suppress the native flyout, or how to hand a
> gesture back to the taskbar from under the shield. Both are settled and
> written up in `docs/LIMITATIONS.md` #22 and #23. Nine approaches were
> measured across the two; the ones that work are what is in the code.

---

## Where things stand

All three taskbar items from `PLAN.md` are built and were verified against the
live taskbar on this machine (build 26200):

- **Hover** an app button and Polish's own window list opens, anchored to the
  button, headed with the app's own name, MRU-ordered, with per-row
  minimize/maximize/close. The native thumbnail flyout never appears.
- **Left-click** a button with 2+ windows and it cycles that app's windows in
  MRU order, over an order frozen at the first click. **Shift+click** walks
  the same list backwards. Verified across three windows: forward 1-2-3-1,
  reverse 3-2-1-3.
- The hovered list marks the app's currently-focused window as selected.
- **Everything else** in the strip still reaches the real taskbar: right-click
  jumplist, middle-click, drag onto a button. Ctrl hands the strip back
  completely, native flyout included. Shift+click is claimed for reverse
  cycling, so native "open a new instance" is middle-click only.
- Tray toggle switches the whole thing off, and off means no shield window
  exists at all.

The files: `src/hook/TaskbarShield.{h,cpp}`, `src/hook/TaskbarHook.{h,cpp}`,
the taskbar section of `src/main.cpp`, plus `name` on `TaskbarButton` and
three additions to `AltTabListWindow` (`SetAnchorRect`,
`SetFooterLegendEnabled`, `SetOnRowHovered`).

**The user commits themselves.** Do not run `git commit`. Offer a message.

## What was measured, so it is not re-litigated

`docs/LIMITATIONS.md` #23 has this in full. The short version, because three
of these look like they should work:

- `HTTRANSPARENT` from `WM_NCHITTEST` **does not cross a process boundary** --
  it is documented as same-thread only. It looks like it works, because
  absorbing the pointer and passing it on are indistinguishable from outside
  when what you are checking is "did the flyout stay away".
- `WS_EX_TRANSPARENT` **does** cross one. That is what the shield toggles.
- Setting that style from inside the mouse hook **does not help the press that
  set it**, even though the style lands within 4ms. The press has to be
  swallowed and replayed.
- The replay **has to be posted**, not sent from the hook. From the hook it
  fails exactly as the original press did.

## What's next, in order

0. **Watch for the native thumbnails reappearing.** They did once, and the
   cause was the shield covering a stale rect after the strip re-centered
   (see `docs/LIMITATIONS.md` #22). That is now event-driven and measured
   at 210ms, under the native 250-450ms dwell. If it is seen again,
   measure the gap before changing anything: hover with a continuous
   glide rather than a teleported cursor, and check the log for how long
   after a `foreground changed` line the next taskbar read lands.

1. **The taskbar's own hover highlight is gone.** The shield owns the pointer,
   so the taskbar never draws it. Either Polish draws its own in the shield
   (it is layered at alpha 1 today, so this means giving it a real alpha and
   painting the button's rect) or this becomes a deliberate "no".
3. **Halo-on-row-hover is wired but unverified.** Every VS Code window was
   covered by a maximized Edge during testing, and `ActiveWindowHalo` pins
   itself directly beneath its target, so a covered window shows nothing --
   the known limitation, not a new bug. Wants a look with a visible window.
4. **Ctrl needs one mouse move to take effect** when pressed with the pointer
   already resting on a button, because pass-through is driven by mouse events
   plus a 100ms poll. Accepted, not fixed.
5. **Secondary taskbars are untested.** One monitor on this machine. The
   shield creates one window per taskbar `HMONITOR` and `AutomationId` repeats
   across taskbars, so this is where a regression would hide.

## Gotchas that cost real time already

- **DPI.** UIA `BoundingRectangle` is virtualized for a DPI-unaware process --
  rects come back at exactly half size on a 200% display, self-consistent and
  wrong. `polish.exe` is Per-Monitor-V2 via its manifest, so it is fine; any
  *test harness* must call `SetProcessDpiAwareness(PER_MONITOR_AWARE_V2)`
  or it will lie convincingly.
- **Shield alpha must be 1, not 0.** A fully transparent layered window is
  excluded from hit-testing: it would receive nothing and block nothing.
- **Swallowing `WM_MOUSEMOVE` in a low-level hook freezes the cursor.**
  Measured. Buttons are fine; only moves.
- **The replay carries a signature in `dwExtraInfo`** and the hook checks it on
  the way in. Without that, a replay is mistaken for a real press and replays
  itself forever, inside a low-level hook. Do not switch this to
  `LLMHF_INJECTED` -- every harness below uses `SendInput`, and a hook that
  ignored injected input could only be tested by hand.
- **Secondary taskbars.** `AutomationId` repeats across taskbars -- the same
  app pinned on two monitors gives two buttons with identical ids. Scope every
  search per taskbar HWND.
- **`polish::GetWindowIconHandle` leaks an HICON** on its shell-fallback path.
  The panel caches per HWND (`g_taskbarIconCache`) to bound it; do not call it
  uncached from anything hover-driven.
- **Explorer restarts** invalidate every taskbar HWND and UIA element.
  `TaskbarCreated` is handled in `main.cpp` and drops the shield and panel.
- **In PowerShell harnesses, `INPUT` must be 40 bytes.** An oversized struct
  makes `SendInput` fail silently and the cursor never moves.
- **Windows Sandbox is wrong for this.** Its screenshot pipeline does not
  render the taskbar. Test on the host.
- **The screen can lock mid-test** and every screenshot comes back black
  without any other symptom. Check the foreground window's class before
  trusting a capture.

## Verify

```
cmake --build --preset default
ctest --preset default          # 96 tests
.\tools\taskbar-probe.ps1       # live button->windows join, read-only
.\run.ps1                       # stops any running instance, rebuilds, launches
```

`tools\check-log.ps1` tails `%TEMP%\polish.log`.

The harnesses that produced the findings above lived in a session scratchpad
and are gone. They were: a hover probe (park the cursor off the strip, move
onto a button, screenshot the area above it), a click probe (same, with a
button press and an optional held Ctrl), a press-watch (send the press and the
release separately, sampling the shield's ex-style and `WindowFromPoint` in
between), and a delay probe (open the shield with Ctrl, wait, then send a
signed press). Any of them is twenty minutes to rebuild from
`docs/LIMITATIONS.md` #22 and #23.

**Always take a control.** The first flyout A/B looked conclusive and was not:
"no flyout" was read as success until the same harness with Polish stopped
showed the flyout appearing, which is what made the Ctrl case's *failure*
legible.

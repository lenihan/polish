# Polish — Build Plan

Living tracker for Polish, a Windows 11 utility that keeps native Snap and
native Maximize/Restore working together correctly: Windows doesn't
update a window's restore position (`WINDOWPLACEMENT.rcNormalPosition`)
when you Snap it, so Maximize-then-Restore after a snap loses it. Polish
fixes that in the background — no new gesture, hotkey, or UI.

See [`README.md`](README.md) for what it does day-to-day and
[`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) for known, permanent
limitations (not bugs to fix). A condensed history of earlier, abandoned
approaches is at the bottom of this file, kept for the Win32 lessons
learned along the way.

## Shipped

- [x] Foreground-window tracking via WinEvent hooks
      (`EVENT_SYSTEM_FOREGROUND`/`MOVESIZESTART`/`MOVESIZEEND`/
      `LOCATIONCHANGE`/`OBJECT_DESTROY`), with drag-gating and a 180ms
      settle debounce so a live drag or a paused mid-drag doesn't produce
      spurious settle events.
- [x] `SyncRestorePlacement`/`SyncRestorePlacementNow` (`src/main.cpp`):
      keeps `rcNormalPosition` synced to the window's last settled rect,
      both reactively (on every settle) and proactively (the moment a
      window is first tracked, so a window that was already snapped
      before Polish started still gets corrected without needing to be
      moved again).
- [x] Tray icon (`src/tray/TrayIcon.*`) with an Exit option; stock system
      icon for now (see Next).
- [x] Single-instance guard, Per-Monitor-V2 DPI awareness, CMake presets,
      unit tests for `RectUtils` (`RectsApproximatelyEqual`).
- [x] **User-confirmed working in real day-to-day use (2026-08-29)** —
      the actual test that mattered, beyond build-verification and
      synthetic repros.

## Next

1. **Real tray icon** — currently a stock system icon; needs a custom one
   (`resources/app.rc` already has the `RT_MANIFEST` embed pattern to
   extend for an `RT_ICON`/`RT_GROUP_ICON`).
2. Full multi-window tracking (sync every window, not just the
   foreground one) — `WinEventHookManager`, a tracked-window set seeded
   via `EnumWindows`, and a periodic reconciliation sweep.
3. Autostart (`HKCU...\Run`, `StartupApproved` awareness).
4. Settings (`%LOCALAPPDATA%` config: global enable/disable, an exclusion
   list by process name).
5. "Restart as Administrator" tray item, so elevated windows (currently
   invisible to Polish — see limitations #1) can be covered too.
6. Full `docs/LIMITATIONS.md` pass and manual test matrix once the above
   land.

## History (condensed)

Polish went through three abandoned UI directions before landing on the
current, UI-free fix — each is described in more detail in git history
if the reasoning ever matters again, but the short version:

1. **A custom always-visible overlay button** drawn next to the minimize
   button. Abandoned: clicking it triggered a spurious foreground-change
   event that broke tracking, and it was the buggiest, most
   Win32-subtlety-laden part of the app.
2. **Shift+click a window's real maximize button** to toggle between its
   last two rects, with a hover-tooltip hint. The toggle itself worked,
   but the hint could never be made reliable — Windows 11's own Snap
   Layouts flyout appears on hover over any maximize button, independent
   of any modifier key, and covers anything drawn in that same spot.
3. **Shift+Right-click anywhere on the title bar → radial (pie) menu**
   (Toggle/Maximize/Minimize/Snap Left/Snap Right), specifically to dodge
   the flyout-collision problem above. Fully implemented and
   build-verified, but abandoned mid-testing when the user reframed the
   actual problem: the annoyance wasn't "no quick way to toggle," it was
   "Windows' own restore position is wrong after a snap" — fixable
   directly, with no new gesture needed at all. `src/hook/RadialMenu.*`
   and `src/hook/TitleBarMenuHook.*` were deleted once that was
   confirmed as the right direction.
4. **Toggle itself (`RectHistory`, `HotkeyManager`, Win+Alt+T)** was kept
   briefly as a secondary keyboard-only action after the radial menu was
   deleted, but once the user decided they didn't need the hotkey either,
   it had no trigger left at all — deleted along with it rather than kept
   as unreachable code.

**Win32 gotchas hit along the way, worth remembering for future work in
this codebase:**

- `SetWindowPos`'s `hWndInsertAfter` places a window *behind* the given
  handle, not in front of it.
- `Gdiplus::Graphics(HDC)` does not reliably preserve alpha into a
  backing DIB; a `Gdiplus::Bitmap` needs to wrap the DIB's own memory
  directly (`PixelFormat32bppARGB`, external buffer via
  `CreateDIBSection`), with alpha manually premultiplied before
  `UpdateLayeredWindow`.
- `app.manifest`'s `dpiAwareness` element must use the
  `http://schemas.microsoft.com/SMI/2016/WindowsSettings` namespace, not
  2017 (that's for `longPathAware`) — using 2017 fails silently, the app
  still launches but falls back to DPI-unaware.
- `TTM_ADDTOOL` (and comctl32 calls generally) silently fail unless
  `app.manifest` declares a Common Controls v6 dependency
  (`Microsoft.Windows.Common-Controls`, version `6.0.0.0`) — without it,
  comctl32 loads against an ABI whose struct sizes don't match what
  current SDK headers expect.
- Don't convert between `GetWindowRect()` and DWM extended-frame-bounds
  when recording/restoring a rect — the invisible resize-border inset
  isn't a fixed per-window constant (it differs for edges flush against a
  Snap boundary vs. free-floating), so a round-trip conversion introduces
  a few pixels of position error. Record and restore raw `GetWindowRect()`
  values directly; no conversion needed.
- PowerShell tool invocations used for manual testing are DPI-unaware by
  default and each is a fresh process — call
  `SetThreadDpiAwarenessContext(-4)` at the top of every script that
  touches window rects, or coordinates get silently virtualized.
- This dev machine has something claiming a broad range of
  `Win+Alt+<letter>` hotkeys — moot now that the hotkey is gone, but
  relevant again if a hotkey is ever reintroduced.

# Changelog

## 0.5.3 — Stacks in the taskbar and Alt+Tab, and a halo fix

**Download:** `Polish-0.5.3-x64.msi`

- **A stack's taskbar button lists its windows.** Hover it for the stack's
  name and its windows, each with its close and minimize buttons.
- **Alt+Tab lists every window in a stack**, grouped under the stack's name
  with the windows indented beneath it. Click the stack to go to its active
  window, or a window to go to that one. Tab still steps through windows in
  most-recently-used order, so a quick Alt+Tab toggles between your last two
  windows whether or not they are in stacks.
- **Fixed: clicking a stack's title bar maximized it** when the stack sat at
  the top of the screen. A click now only brings the stack forward.
- **Fixed: the halo around a stack** went around the active window alone and
  spilled over a stack or window tiled beside it. It now wraps the whole stack
  and stops at its neighbour.

## 0.5.2 — Stacks and apps come back after a restart

**Download:** `Polish-0.5.2-x64.msi`

- **Stacks survive a restart.** Polish remembers each stack's windows, tab
  order, name, tab-strip side, size and position, and rebuilds it when it
  starts. If the apps are still launching it keeps trying for two minutes.
- **Reopen your apps at start-up.** A few seconds after Polish starts, a
  dialog lists the windows that were open last time and are not open now, each
  with a checkbox, all ticked. OK opens the ticked ones, Cancel opens none.
  Apps reopen with the command line they were started with, so documents
  usually come back; Store apps open by app ID. File Explorer windows and apps
  running as administrator are not included, and window positions are not
  restored (stacks excepted). Turn it off in Settings.

## 0.5.1 — Win+Shift+S works again

**Download:** `Polish-0.5.1-x64.msi`

- **Fixed: Win+Shift+S (screenshot) showed Polish's move mode** on PCs where
  the keyboard reports real keystrokes as injected, such as remote desktop or
  key-remapping setups. Pressing Shift or any other key during a Win hold now
  hands the hold back to Windows, and a click no longer grabs a window once it
  has.

## 0.5.0 — Stack snapping and previews, a new icon

**Download:** `Polish-0.5.0-x64.msi`

- **Stacks snap to the screen edges.** Drag a stack by its title bar to the
  left or right edge for half the screen, a corner for a quarter, or the top
  for the whole work area. A preview shows where it will land.
- **Hover a stack tab for a thumbnail.** Rest the pointer on a tab whose
  window is behind the active one and a live thumbnail appears.
- **Stacks have their own taskbar icon**, three cascaded window cards, instead
  of the Polish icon.
- **A new Polish icon**: a single window card. At tray size it is a white
  frame and title bar around a blue app area, so it reads as a window on a
  dark taskbar.
- **Win+Space accepts the mouse.** After latching a window with Win+Space you
  can drag it with the left button as well as use the arrow keys.
- **The tray menu says how to move windows**: "Hold Win and left-drag to
  move/resize any window", and Win+Space beside "Move or resize a window".
- **Fixed: right-clicking Start or a tray icon** opened nothing. Polish was
  swallowing the click.
- **The Polish button beside Start is off by default.** Turn it on from the
  tray menu if you want it.

## 0.4.3 — Taskbar buttons work while an administrator app is in front

**Download:** `Polish-0.4.3-x64.msi`

- **Clicking a taskbar button no longer does nothing while an app running as
  administrator has focus.** Windows discards input that a normal process
  injects while an elevated window is in front, which is how Polish hands a
  click back to the taskbar. In that state Polish now steps aside: the
  taskbar gets the real click. The cost is that Windows' own hover preview
  shows, and click-to-cycle is off, until a normal window is in front again.
  "Restart as Administrator" in the tray menu avoids the tradeoff.

## 0.4.2 — Accent colour on PCs where the registry accent is grey

**Download:** `Polish-0.4.2-x64.msi`

- **The halo and other accent-coloured overlays now follow the accent
  palette.** On some PCs the registry values Polish read for the accent
  stay a neutral grey while the accent Windows actually shows is blue, so
  the halo came out grey. The palette Windows itself uses is read first.

## 0.4.1 — Accent colour fix, and drag to reorder taskbar buttons

**Download:** `Polish-0.4.1-x64.msi`

- **The halo and other accent-coloured overlays now use the accent you picked
  in Settings.** They read the title-bar accent, which stays a neutral grey
  when "Show accent color on title bars" is off, so the halo came out grey
  on those PCs.
- **Taskbar buttons can be dragged to reorder again.** Polish was claiming
  the press on any app with two or more windows, so the taskbar never saw
  the start of a drag. A click still cycles the app's windows, now on
  release; a drag goes to the taskbar.

## 0.4.0 — Settings, scaled windows, stacks and a Polish taskbar button

**Download:** `Polish-0.4.0-x64.msi`

- **Settings window** with a checkbox and description for every feature, a
  Shortcuts section for the global hotkeys, Start with Windows, and
  scaled-window options. Follows the system dark/light theme.
- **Scaled windows:** shrink a window to a live thumbnail and click to
  restore it. Works with Alt+Tab, the taskbar panel and stacks.
- **Frozen apps** support.
- **Stacks** replace Groups, with a title bar and tab strip, n-way tiling,
  and tile/cascade for every window on a monitor.
- **Easy move/resize:** hold Win to drag any window from anywhere, with snap
  to halves and a layout menu (Ctrl+Alt+Space).
- **Polish taskbar button** beside Start, and a Layout button.
- **Halo and bullseye** are drawn in your accent colour; the bullseye ignores
  empty copies. New app icon.
- **Tray:** keyboard-reachable menu, Restart as Administrator, and the build
  stamp in the tooltip.
- **Fixes:** the halo could disappear on every window until restart after a
  minimized window was closed; stray specks at the halo's corners; the taskbar
  overlay swallowed clicks while a full-screen app covered the taskbar.

## 0.3.0 — Alt+Tab follows the monitor you are on

**Download:** `Polish-0.3.0-x64.msi`

- **Alt+Tab is scoped to the active monitor.** The list used to hold every
  window on every display, ordered so the current monitor came first, which
  meant tabbing off the end of one monitor's windows walked onto the next
  monitor's. It now holds only the windows on the monitor you are working
  on, so reaching the end wraps back to that monitor's first window. Each
  display still gets its own list panel, but only the active one appears.

  The tradeoff: a window on another monitor is not reachable from Alt+Tab.
  Move focus to that monitor first.

Everything from 0.2.0 is unchanged. On a single-monitor PC this release
behaves exactly like 0.2.0.

## 0.2.0 — first installer build

The first version of Polish packaged for other people to install: an x64
MSI rather than a bare executable. Feature work to date is in
[`PLAN.md`](PLAN.md); this file starts here.

The previous release, `v0.1.0`, shipped an ARM64 `polish.exe` that will not
run on an ordinary Intel or AMD PC. This build is x64 and runs everywhere.

**Download:** `Polish-0.2.0-x64.msi`

### What Polish does

- **Alt+Tab, rebuilt** — a window list in most-recently-used order, with
  Alt+` for switching between one app's document tabs.
- **Taskbar hover** — hovering an app's taskbar button shows Polish's own
  window list instead of the native thumbnail flyout, with per-row close,
  minimise and maximise. Clicking a button cycles that app's windows.
- **Click empty taskbar** — opens the window switcher and steps through it,
  Shift to reverse. Right-click does the same for the current app's tabs.
- **Active window halo** — a soft glow around whichever window has focus.
- **Window groups** — combine several windows into one tabbed or tiled
  frame.
- **Restore-position sync** — fixes Windows not updating a window's restore
  rectangle after a Snap, so maximise-then-restore behaves.
- **Copy/paste bullseye** — a ring animation showing where a copy or paste
  landed.

Every feature can be switched off individually from the tray menu, and
switching one off restores Windows' own behaviour completely.

### Installing

Windows will show **"Windows protected your PC"**, and your browser may warn
that the file isn't commonly downloaded. Polish isn't code-signed yet, so
Windows has nothing to identify the publisher by. Click **More info** →
**Run anyway**. Signing is the next milestone and removes this.

Installs to `C:\Program Files\Polish` and needs the admin prompt. Uninstall
from **Settings → Apps → Installed apps**, which also removes its settings
and startup entry.

### Requirements

Windows 11, x64. An ARM64 machine runs this build under emulation. On
Windows 10 the taskbar features stand down and the rest still works.

### Known limitations

- While the Start menu or Search is open, Windows lifts the taskbar above
  Polish's overlay, and the native thumbnail flyout reappears over Polish's
  own window list. The fix needs a code-signed build — the next milestone.
- Elevated windows (Task Manager, anything "run as administrator") can't be
  managed, and never will be: a normal-privilege process is not allowed to.
- Some apps expose only part of their tab list to Alt+`; see
  [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md).

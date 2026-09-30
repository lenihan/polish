# Changelog

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

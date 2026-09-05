# Polish — Build Plan

Windows 11 utility that keeps native Snap and Maximize/Restore working
together correctly, plus a replacement Alt+Tab and a window-grouping
(tab/tile) feature. See [`README.md`](README.md) for day-to-day behavior
and [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) for known, permanent
limitations. Full history (design decisions, bugs found and fixed, Win32
gotchas) lives in git history on this file, not here — this is just the
current todo list.

## Done

- Restore-position sync: fixes Windows not updating a window's restore
  rect after a Snap, so Maximize-then-Restore works correctly.
- Tray icon, single-instance guard, DPI awareness, autostart, and a
  settings menu with per-feature toggles.
- Statically linked the CRT so the app runs on a bare Windows machine
  without needing the VC++ Redistributable installed separately.
- Alt+Tab replacement: MRU-ordered cycling with a dim overlay and
  highlight border instead of native switcher, skipping minimized
  windows.
- Alt+Tab: candidate list live-updates for opened/closed windows but
  keeps a stable order while cycling.
- Alt+Tab: highlight border is a thin solid line that matches the
  screen's own rounded corners on maximized/fullscreen windows.
- Alt+Tab: a list panel shows every candidate window with the current
  selection highlighted, in normal-sized text.
- Alt+Tab: multi-monitor support — one panel per monitor, Tab flows from
  one monitor's windows into the next, panels rebuild automatically when
  monitors are connected or disconnected.
- Alt+Tab: fixed a bug where a hidden, title-less UWP host window could
  get selected instead of the real app (e.g. Settings).
- Window groups (tab/tile mode): merged to main — combine multiple real
  windows into one taskbar entry, switchable via tabs or shown as tiles.
- Groups: real window reparenting so members can't be dragged out
  accidentally.
- Groups: user-resizable tile splitters with double-click-to-reset.
- Groups: a management dialog with active/member lists and
  drag-to-reorder.
- Groups: configurable "New Group" hotkey (works around hotkey
  conflicts on this machine).
- Groups: tab-hover thumbnail preview of a member window.

## Left to do

- Alt+Tab: add a minimized-windows section to the list panel, separate
  from the main cycle.
- Alt+Tab: add per-row minimize/close action buttons on the highlighted
  row.
- Alt+Tab: diagnose a visual "shadow" left behind on a window after
  tabbing away from it.
- Alt+Tab: confirm whether Settings still fails to come to front after
  the phantom-window fix, or if that report was multi-monitor-related.
- Groups: handle a member window closing while backgrounded or tiled.
- Groups: test and fix multi-monitor / mixed-DPI support (never tried on
  real hardware).
- Groups: clamp tile auto-grow-to-fit to monitor bounds.
- Groups: build the custom title bar with alignment/mode/manage-windows
  controls (riskiest planned piece, not started).
- Groups: make tab placement and tile-grid growth direction
  orientation-aware.
- Groups: rename to "Polish Groups" and make it pinnable to the Start
  menu.
- Groups: support nested groups (a group containing other groups).
- Groups: add Alt+backtick MRU switching scoped to one group's members.
- Groups: add a per-process exclusion list.
- Add a "Restart as Administrator" tray item so elevated windows become
  manageable.
- Do a full `docs/LIMITATIONS.md` pass and manual test matrix.
- Backlog, not yet scoped: clipboard copy flash, paste history popup,
  Quick Access rename without renaming the file, radial start menu,
  taskbar icon click-to-cycle, reorder windows within a taskbar group,
  consider a WinUI3 rewrite, improve virtual desktops (persist layout,
  pin an app to all desktops, show current desktop indicator).

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
- Alt+Tab: the highlighted candidate gets the same theme-aware halo as
  the active window (`ActiveWindowHalo`, shared instance), replacing the
  earlier thin blue border; `AltTabHighlightBorder` remains only for the
  group active-tile ring.
- Alt+Tab: a list panel shows every candidate window with the current
  selection highlighted, in normal-sized text.
- Alt+Tab: multi-monitor support — one panel per monitor, Tab flows from
  one monitor's windows into the next, panels rebuild automatically when
  monitors are connected or disconnected.
- Alt+Tab: fixed a bug where a hidden, title-less UWP host window could
  get selected instead of the real app (e.g. Settings).
- Alt+Tab: a separate "Minimized" section (below an "Active" heading list)
  reaches minimized windows via Up/Down without joining the Tab cycle.
- Alt+Tab: per-row minimize/maximize/close icon buttons on the
  highlighted or hovered row, plus Del/-/+ keyboard equivalents for the
  highlighted row, documented via an on-screen footer legend.
- Alt+Tab: a monitor's panel caps its height and scrolls (keeping the
  highlighted row in view) with a chevron+count strip ("▾ N more") when
  there are more candidates than fit on screen, and the keyboard-shortcut
  footer stays pinned in view instead of scrolling away.
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
- Groups: replaced the native OS title bar with a self-painted one
  (shrink-the-caption technique, keeps DWM shadow/rounded corners/Snap)
  with working minimize/maximize/restore/close buttons; verified live at
  2x DPI.
- Groups: mode-toggle and manage-windows buttons in the title bar
  (reusing the existing callbacks behind the right-click menu, which
  stays as a redundant path) — client-area buttons, not caption ones,
  with their own glyphs (grid/tab icon, list icon) and hover highlight;
  verified live that both fire correctly and never start a window drag.
  Alignment button still deferred until tab/tile orientation-aware
  layout lands.
- Groups: vertical-alignment tab strip now gets the same Win11 "grows
  out of the body" treatment as horizontal — a full-height connector
  column (mirroring the horizontal connector band), the active tab's
  content-facing edge square/unbordered instead of rounded, and concave
  fillets joining the tab to the column on both sides. Tab placement and
  tile-grid growth direction were already orientation-aware; this closed
  the one remaining visual gap (`DrawConcaveFillet` generalized to work
  on either axis).
- Groups: added a third layout mode, **Stack** — every member visible at
  once like Tile, but forced to a single row (Horizontal alignment) or
  single column (Vertical alignment) instead of a roughly-square grid.
  Cycles in with the title-bar mode button (Tab → Tile → Stack → Tab);
  the right-click context menu now shows all three as a radio group
  instead of one cycling item. Reuses Tile's splitter/tile-maximize/
  active-tile-ring machinery (`GroupMode::Tile`/`Stack` share
  `IsTiledMode`) — only the grid shape itself differs
  (`ComputeGridShape`).
- Groups: a member's position/size is now actively enforced, not just
  set once — dragging or resizing a member's own frame inside the group
  (revealing other members Z-ordered behind it in Tab mode, or sliding a
  member over the tab strip) is cancelled (`WM_CANCELMODE` on
  `EVENT_SYSTEM_MOVESIZESTART`) and snapped back
  (`GroupManager::EnforceMemberRect`, driven off
  `EVENT_OBJECT_LOCATIONCHANGE`/`EVENT_SYSTEM_MOVESIZEEND`) to the rect
  `ApplyLayout` last assigned it.
- Groups: the picker's selection now stays in the list a run of adds/
  removes is happening in (the row that took the moved window's place),
  instead of jumping to the other panel after every single one.
- Groups: UWP/Store app windows (Calculator, Settings, Photos, ...) are
  refused outright rather than joined — `SetParent` categorically fails
  for the `ApplicationFrameWindow` class (confirmed live,
  `ERROR_INVALID_PARAMETER` every time). The picker lists such a window
  greyed out, unaddable, with a tooltip explaining why
  (`WindowFilters::IsUnreparentableWindow`); `GroupManager::ApplyLayout`
  drops one defensively if it ever arrives some other way. A prior
  top-level "attached" mode was tried and abandoned — its taskbar button
  couldn't be hidden by any mechanism (OS-level restriction), and there
  was an unresolved Tile/Stack sizing glitch — see `docs/LIMITATIONS.md`.
- Active window halo: a soft white/black (theme-aware) glow around the
  focused window's own edge, fading out over ~a quarter inch, so it's
  obvious at a glance which window has focus (`ActiveWindowHalo`). No
  halo on a maximized/full-screen window; during an Alt+Tab session the
  same halo instance moves to the highlighted candidate instead;
  follows focus across virtual desktops and theme changes live; toggle in
  the tray menu. Deliberately a separate class from `AltTabHighlightBorder`
  (see its own class comment), Polish's first continuously-rendering
  overlay, using a hand-written per-pixel signed-distance-field rasterizer
  (not GDI+) for perf reasons — measured cost is imperceptible (sub-tick)
  even at roughly half-screen target size.

## Left to do


- Groups: Active window title should look very different than inactive
- Need a more unique icon...current icon looks like Google Gemini
- Add Alt+` to cycle a single app's own windows by most-recently-used
  order (native Windows does this by Z-order, not MRU).
- Groups: handle a member window closing while backgrounded or tiled.
- Groups: test and fix multi-monitor / mixed-DPI support (never tried on
  real hardware).
- Groups: clamp tile auto-grow-to-fit to monitor bounds.
- Groups: rename too "Polish Groups" and make it pinnable to the Start
  menu.
- Groups: support nested groups (a group containing other groups). The
  picker now *lists* other groups' chrome windows and lets you add one
  (it reparents like any other window), but layout treats it as a plain
  member — `GroupMemberKind::NestedGroup` is still never populated.
- Groups: add Alt+backtick MRU switching scoped to one group's members.
- Groups: add a per-process exclusion list.
- Groups: keyboard shortcuts for the group window itself (switch tabs,
  cycle mode, alignment, tile-maximize). Needs the low-level keyboard
  hook the way Alt+Tab does, not a plain WM_KEYDOWN handler: with a
  group open, focus belongs to the *member app* (a foreign process), so
  the chrome window never receives keystrokes at all.
- Tray menu: reachable by keyboard (NIM_SETVERSION + NIN_KEYSELECT so
  Win+B finds it, menu positioned at the icon rather than the cursor,
  and `&` mnemonics on the items).
- Add a "Restart as Administrator" tray item so elevated windows become
  manageable.
- Add an "easy move/resize" mode: while active, the whole window is a
  move target (not just the title bar), and resizing snaps to screen/
  other-window edges. Resizing area should be finger friendly. 
  Slick ui that flips windows around with move/resize controls
- Taskbar: clicking a taskbar app icon should cycle through that app's
  windows in MRU order (native Windows does this by Z-order, not MRU) —
  same MRU-ordering idea as the Alt+backtick item above.
- Taskbar: hovering a taskbar app icon should show its windows with
  thumbnails for active ones and plain text for minimized ones.
- Taskbar: that hover preview should let you minimize/maximize/restore/
  close each window directly, without switching to it first.
- Virtual desktops: remember which apps were on a desktop and offer to
  reload them.
- Virtual desktops: an app pinned to show on all desktops should keep
  that setting (persist it, not just apply it once).
- Virtual desktops: show an indicator of which desktop you're currently
  on.
- Virtual desktops: use visuals generally to make the whole concept
  easier to understand (exact treatment still unscoped).
- Add mouse "sticky" to keep pointer on app rather than traveling to next
  screen. Like for snapping and trying to access tools on the border.
- Add mouse "sticky" for hidden taskbar to keep pointer on app rather than 
  making taskbar unhide
- Do a full `docs/LIMITATIONS.md` pass and manual test matrix.
- Backlog, not yet scoped: clipboard copy flash, paste history popup,
  Quick Access rename without renaming the file, radial start menu,
  reorder windows within a taskbar group, consider a WinUI3 rewrite.

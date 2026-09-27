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
- Alt+Tab: opens whenever there is any window at all, rather than
  requiring two non-minimized ones — one active window still gets its
  row's minimize/maximize/close buttons, and zero active windows is
  exactly when reaching a minimized one matters most. Only a desktop
  with nothing active *and* nothing minimized hands off to native
  Alt+Tab. With no active windows, Tab/Shift+Tab cycle the Minimized
  section itself (starting on its first row, since there is no current
  window at index 0 to skip).
- Taskbar: established how far a non-injecting process can get. Buttons
  are readable via UI Automation (class
  `Taskbar.TaskListButtonAutomationPeer`, with the AppUserModelID in
  `AutomationId` behind a literal `"Appid: "` prefix), and a button maps
  to its windows exactly via `IApplicationResolver::GetAppIDForWindow` --
  *not* via the documented `SHGetPropertyStoreForWindow`, which returned
  nothing for most ordinary Win32 windows. Two things that look like they
  should work do not: the legacy `MSTaskListWClass` chain still exists but
  is a dead stub (`TB_BUTTONCOUNT` returns 0), and both hit-test APIs
  (`ElementFromPoint`, `AccessibleObjectFromPoint`) refuse to descend into
  the XAML island, so hit-testing must be done by hand against cached
  rects. `tools/taskbar-probe.ps1` checks the whole mapping against the
  live taskbar.
- Taskbar: hovering an app button shows Polish's own window list --
  MRU-ordered, headed with the app's own name, with the per-row
  minimize/maximize/close buttons the Alt+Tab panel already had, and
  anchored to the button it belongs to. The native thumbnail flyout never
  appears at all: a layered topmost "shield" over the button strip owns
  the pointer there, so the taskbar never gets a pointer-enter and its
  hover dwell never starts (`docs/LIMITATIONS.md` #22). Hovering a row
  halos the real window where it actually sits.
- Taskbar: left-clicking a button with 2+ windows cycles that app's
  windows in MRU order, over an order frozen at the first click so
  repeated clicks walk the whole list instead of ping-ponging between
  the two most recent. 0 or 1 windows is left entirely to Windows.
- Taskbar: shift-clicking a button with 2+ windows cycles the same list
  backwards, the way Shift reverses Alt+Tab. That claims the native
  shift+click (open a new instance), which middle-click still does.
- Taskbar: ctrl-clicking jumps to the most recently used *other* window,
  so repeated ctrl-clicks toggle between the two most recent -- the tap
  half of the Alt+Tab split, against click-to-cycle's hold half. Read
  from `ActivationHistory` live rather than from the button's cached
  list, which is only as fresh as the last taskbar read and would go
  stale under a fast repeat. This narrows the Ctrl escape hatch to
  everything except the left button: Ctrl+hover still shows the native
  flyout and Ctrl+right/middle-click still reach the taskbar.
- Taskbar: hovering a button marks the app's currently-focused window as
  selected in the list, so it says where you already are before it says
  where you could go. Nothing is marked when the foreground window
  belongs to another app.
- Taskbar: the hover panel's per-row minimize/maximize/close buttons work,
  light up under the pointer (close in Windows' own close-red, the two
  toggles in a plain lift) and name themselves with a tooltip. They were
  wired to the Alt+Tab panel's handlers at first, every one of which opens
  `if (!g_altTabSessionOpen) return;` -- so all three silently did nothing,
  the click being dropped on the handler's first line.
- Taskbar: any row action that changes whether a window is minimized
  rebuilds the panel rather than repainting the row -- minimize, Normal,
  and maximize alike. A repaint redraws the row where it already is, so a
  window that had just been maximized out of the Minimized section stayed
  sitting under that heading until the panel was closed and reopened.
  Maximize was the last path still taking the cheap repaint when it could
  not.
- Taskbar: minimizing a window from the hover panel moves its row down
  into the Minimized section straight away, and if it was the window in
  front, focus moves to that app's next non-minimized window rather than
  dropping to whatever happened to be behind it. This is the one case
  where rows are deliberately allowed to move under the pointer -- the
  reshuffle is what the gesture asked for, unlike the MRU reordering the
  panel goes out of its way to suppress.
- Taskbar: the hover panel is keyboard-drivable with the same keys as
  Alt+Tab -- Up/Down and Home/End/PageUp/PageDown move the selection
  (previewing as they go), Del/-/+ act on the selected row, Escape puts
  the previewed window back and closes. Enter and N are additions: the
  panel has no Alt to release, so "keep this one" needs a key, and N
  opens another window of the app. It borrows those keys from
  AltTabHook's existing keyboard hook rather than installing a second
  one (`SetExternalSessionActive`); an Alt+Tab session wins if both are
  somehow showing, since it is the more deliberate gesture.
- Taskbar: the panel's first entry is a "New window" command row. Packaged
  apps are started through their real AppUserModelID; everything else
  through the executable behind one of its existing windows, because a
  plain Win32 app's AUMID is shell-synthesized and ActivateApplication
  rejects it.
- Taskbar: click-to-cycle skips minimized windows -- cycling is for moving
  between the windows in front of you, and landing on a minimized one
  turns a switch into an un-minimize nobody asked for. They stay
  reachable by clicking their row, which is the gesture that says "this
  one". An app whose windows are all minimized restores the most recent.
- Row buttons (taskbar and Alt+Tab alike): close, minimize and maximize on
  every row, minimized ones included. The maximize button used to be left
  off a minimized row on the reasoning that there is no maximized state to
  toggle -- which confused "what state is it in" with "where can it go". A
  minimized window is coming back either way; the two buttons are how you
  say at which size.
- Row buttons: "Normal" rather than "Restore", and its own action on its
  own key rather than a state the other two toggle into. Shortcuts are
  Backspace close, `-` minimize, `+` (the `=` key, shifted or not)
  maximize, `0` normal, each named in its button's own tooltip. Normal
  uses SW_SHOWNORMAL, not SW_RESTORE: restoring a window minimized *from*
  maximized brings it back maximized, which is the other button's job.
- Row shortcuts (taskbar and Alt+Tab alike) act on the row under the
  pointer when that is the more recent choice, so a window can be closed
  or resized without first clicking it and making it current. Not
  unconditionally, though: the pointer is almost always resting on some
  row while either panel is up, so "hover always wins" would hijack the
  arrow keys. Whichever input last chose a row wins.
- Row shortcuts: a hover notification only counts as a choice when the
  cursor has actually moved. Arrowing through the list previews each row
  by activating its window, which can relayout the panel, and moving a
  window under a stationary pointer makes Windows deliver a WM_MOUSEMOVE
  indistinguishable from a real one -- so the keyboard was handing
  priority back to the mouse by accident, and which input won depended on
  whether a relayout happened to occur.
- Row buttons: a minimized row draws maximize / normal / close, not
  maximize / minimize / close. The middle button used to keep the native
  minimize mark in both states on the reasoning that it stays "the
  minimize control" -- but on a minimized row it no longer minimizes, so
  the mark described an action the button does not perform. It now draws
  the two-overlapping-squares normal glyph there, which is what its
  tooltip has said since Normal got a name of its own.
- Taskbar: an open panel also rebuilds when a row's *minimized state*
  changes, not only when the window set does. Minimizing changes no
  window's existence, so a membership comparison alone cannot see it, and
  a window minimized from its own title bar stayed listed as running in a
  panel that was already on screen.
- Taskbar: the maximize and Normal buttons bring their window to the
  front and leave the panel's selection on it. Resizing a window you
  cannot see is not much use, and the selected row means "the window in
  front", so leaving the selection elsewhere had the panel contradict
  what had just happened. They also cancel any preview in progress --
  clicking a button is a choice, and ending the preview instead would put
  the previously-front window back when the pointer left, undoing it.
  The minimize button deliberately does the opposite and moves focus off
  the window, to the app's next one.
- Taskbar: the shield only ever opens while the pointer is over the
  button strip it covers. It used to open on any mouse button being down,
  wherever the pointer was -- so clicking a row in the hover panel opened
  it, and moving back down to the strip then handed the pointer to the
  real taskbar and brought its flyout back on top of Polish's own list.
  Opening the shield anywhere else buys nothing, since it covers nothing
  there, and costs exactly that.
- Taskbar: a failed or empty taskbar read no longer uncovers the strip. It
  keeps the last-known-good shield, retries in 250 ms, and only degrades
  after three bad reads in a row; `TaskbarCreated` leaves the shield in
  place; a refresh requested mid-read is remembered rather than dropped.
  This was a real, ordinary cause of the native flyout reappearing --
  about ten flyout dwells of exposure per failed read, worst right after an
  explorer restart. See docs/LIMITATIONS.md #24.
- Taskbar: the shield survives the Start menu, via a UIAccess build.
  While Start or Search is open the shell raises the taskbar into the
  MOGO z-band (6), above an ordinary topmost window, and the native
  flyout came back on top of Polish's list. A process holding a UIAccess
  token has its windows placed in the UIACCESS band (2), which outranks
  it -- measured on 26200.9457: taskbar 6, shield 2, `WindowFromPoint`
  over a button still the shield. So `polish_uia.exe` is a second target
  built from the same sources with `uiAccess="true"`, signed and
  installed under `%ProgramFiles%` (`tools/uiaccess/`, `docs/UIACCESS.md`);
  `polish.exe` stays `asInvoker` for development. Verified live with a
  real Start click, an explorer restart, right/middle-click and the Ctrl
  escape hatch. This replaces the Windhawk plan that stood here --
  nothing is injected into explorer.exe.
- Taskbar: an open hover panel is a live view -- opening or closing a
  window of the app whose list is on screen adds or removes a row,
  rather than leaving a header that disagrees with its own rows. Its row
  *order* is held still while it is open, though: the underlying list is
  MRU and activating a window reorders it, so refreshing from it made the
  row you just clicked jump to the top and the list re-sort under the
  pointer. A fresh open takes MRU order; a refresh keeps what is on
  screen and appends anything new at the end.
- Taskbar: with the panel open, a click walks *that* list rather than a
  separately frozen one -- "the next window" means the next row down,
  which is the only thing a click on a visible list can honestly mean.
  The frozen-order session still backs clicks made with no panel open.
- Window filters: `IsCandidateWindowShape` accepts `WS_CAPTION` *or*
  `WS_THICKFRAME`, not `WS_CAPTION` alone. Requiring a caption silently
  excluded real windows from everything Polish does -- found via a
  Copilot window that the taskbar counted and Polish did not, which made
  its hover panel read "3 running windows" over two rows and made that
  window unreachable by Alt+Tab. Custom-frame apps draw their own title
  bar and omit the style; a resizable top-level window is a real one
  either way. See the comment in `WindowFilters.cpp`.
- Taskbar: every other gesture in the strip still reaches the real
  taskbar -- right-click jumplist, middle-click, drag onto a button --
  by swallowing the press, opening the shield and replaying it a
  message-loop turn later. Three things that look like they should do
  this and do not are in `docs/LIMITATIONS.md` #23. Holding Ctrl hands
  the strip back completely, native flyout included.
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
- Bullseye: a large soft theme-aware ring plays on top of all windows
  (`BullseyeOverlay`, `WS_EX_TOPMOST`, click-through) at the point of a
  copy (collapses onto it) or paste (expands out of it) — Polish's first
  animated overlay, `QueryPerformanceCounter`-timed off a 16 ms tick.
  Copy is detected with a clipboard listener (`WM_CLIPBOARDUPDATE`, so
  right-click > Copy counts, filtered to the foreground app's own
  writes, where "own" includes parent/child processes so WebView2-hosted
  apps like the new Outlook count); paste rides the existing keyboard hook (`AltTabHook::
  SetOnPasteChord`, Ctrl+V / Shift+Insert, never swallowed). Anchor is
  the centre of the selected text where there is a selection, else the
  caret via MSAA `OBJID_CARET`, else the caret via a UIA collapsed range
  (cloned and stretched a character so it has a measurable rect), else
  caret → mouse → window centre (`ResolveInteractionAnchor`). All resolved
  on `UiaWorker`'s thread at 4-12 ms with a 90 ms timeout. The three-API
  ladder is not over-engineering: no single one of them works everywhere,
  and the order matters because the last lies in Chromium
  (docs/LIMITATIONS.md #21). Ring is two-tone -- a
  theme-coloured core plus a contrasting edge -- so it reads against any
  background, which one colour could not. Tray toggle. Tuning constants
  (radius/stroke/duration/alpha/outline) are adjusted by eye live.
- Alt+`: the Alt+Tab switcher over the *tabs of the foreground window*
  (`SessionKind::Tabs`, no second hook -- `g_instance` is a single
  static). Tabs are read through UI Automation on a dedicated MTA worker
  thread (`UiaTabWorker`): ~50ms per read, far too slow for the hook
  thread and visible as a stall on the UI thread. Which containers hold
  *document* tabs is per-app data (`TabSwitching.h`) and fails closed --
  probed live: VS Code, File Explorer, Terminal, Notepad, Edge work;
  Outlook and OneNote expose only ribbon tabs and are excluded on
  purpose. Ordered most-recently-used (per-window, keyed on UIA runtime
  ids) so a double-tap toggles, learned by observing which tab is
  frontmost at each enumeration rather than by holding a live listener.
  Verified live.
- Halo: fixed the halo vanishing behind other windows. It asked for
  `HWND_TOP`, which Windows silently declines for a background process
  against the *foreground* window -- returning success while changing
  nothing, so the halo stalled several Z slots behind its own target and
  was buried under whatever was on screen. Now pinned directly beneath
  its target. Also fixed the halo sitting out the grow-from-taskbar
  restore animation: the re-render debounce was re-armed by every
  animation frame and so could never fire. The halo now holds off for
  `kHaloRestoreDelayMs` while a restore animation plays (it cannot be
  followed -- see docs/LIMITATIONS.md #20), started by whichever of the
  foreground change or `EVENT_SYSTEM_MINIMIZEEND` arrives first: they
  race, and the delay was silently doing nothing whenever the foreground
  change won and drew the halo before the suppression flag was set.
- Halo: fixed a faint, uneven line hugging the edge of a haloed window,
  which made a perfectly straight edge read as slightly wavy (with the
  halo off, the same edge looked fine). The glow stopped painting exactly
  at the target's own edge and halved alpha in that last column to
  antialias the seam -- but a Windows 11 window's frame border is partly
  transparent, so the dimmed column showed through it and let the
  wallpaper behind vary the line's brightness down the length of the
  edge. The glow now underlaps its target by `kUnderlapDip` at full peak
  alpha (those pixels sit behind the target, so only its own border ever
  samples them), giving that border a uniform backdrop. Wants a live
  look before this moves out of "needs verifying".
- Halo: an elevated window gets a halo that is actually on top of the
  windows it covers. The halo pins itself directly beneath its target,
  which needs the target's HWND as `hWndInsertAfter` -- and UIPI refuses
  that reference across integrity levels: against Task Manager,
  `SetWindowPos` returned FALSE with `GetLastError` 5
  (ERROR_ACCESS_DENIED), measured. Nothing handled the failure, so the
  halo stayed wherever it last was, which is to say underneath the very
  windows the target was covering -- reported as "I alt-tab to it and the
  halo is behind other windows". It now falls back to HWND_TOPMOST, the
  only placement left that holds (HWND_TOP silently does nothing against
  the foreground window, and an elevated window cannot be raised by this
  process either). The cost is that the glow then sits above its target,
  so the couple of pixels it deliberately underlaps paint over that
  window's own border; only elevated targets pay it. Switching back to an
  ordinary window clears topmost on its own -- a non-topmost
  `hWndInsertAfter` does that by definition -- which was verified live
  along with the z-order in both states.
- Taskbar: Polish draws the hover highlight the shield took away. The
  taskbar lights the button under the pointer by *receiving* the pointer,
  which is exactly what the shield stops, and there is no API to ask
  explorer for it -- so the shield draws it: a faint rounded fill inset a
  few pixels inside the button, light-on-dark or dark-on-light by the
  taskbar's own theme (SystemUsesLightTheme, which is a different setting
  from the app theme the rest of Polish follows). Drawn by the shield
  rather than a window of its own because the shield is already exactly
  over the strip, already above the taskbar, and already in the band that
  wins while Start is open. That meant moving it from a window-wide
  LWA_ALPHA to per-pixel alpha (UpdateLayeredWindow): every pixel stays at
  alpha 1, invisible but still in the hit-test, with the highlight painted
  over it. Nothing is drawn while the taskbar has the pointer back --
  Ctrl held, or the shield bypassed -- since the taskbar lights the button
  itself then and two highlights would stack.
- Taskbar: clicking empty taskbar drives Alt+Tab with no keyboard. The
  first click opens the window switcher, each further click is one Tab
  press and Shift+click one Shift+Tab, and moving the pointer off the
  taskbar commits the highlighted window -- the mouse's version of
  releasing Alt. Because committing puts that window at the front of the
  MRU order, leaving and clicking again lands back on the one you came
  from, so two windows can be alternated by clicking the taskbar alone.
  Escape cancels, Enter commits, a press on an app button stands the
  session down. "Empty" is decided by a UIA read of the taskbar's own
  button rects at click time (~30ms): `ElementFromPoint` stops at the
  taskbar's top-level pane and never reaches the XAML buttons inside it,
  so the rects are the only honest answer, and Start and the tray icons
  keep their own behaviour.
- Taskbar: right-clicking empty taskbar does the same for Alt+` -- the
  tab switcher for the app that was last in front, cycled by further
  right-clicks and reversed with Shift. The app is taken from the
  activation history rather than `GetForegroundWindow`, which by then
  answers "the shell": clicking the taskbar takes the foreground away.
  Committing raises that window along with the chosen tab. This is the
  one gesture that is swallowed rather than replayed, since the native
  taskbar context menu would otherwise open on top of the list;
  Ctrl+right-click still reaches it.

## Left to do
- Halo: When an active app closes, it's halo stays
- Taskbar: the shield tracks the strip via taskbar LOCATIONCHANGE events
  with a 120ms debounce, measured at 210ms from a window appearing to the
  shield being correct -- under the native flyout's 250-450ms dwell, so
  the flyout cannot appear in the gap. If that dwell is ever shortened by
  an OS update, or the debounce is raised, the intermittent-thumbnails bug
  comes straight back. See docs/LIMITATIONS.md #22.
- Taskbar: the Ctrl escape hatch needs the pointer to move once after
  Ctrl goes down. The shield's pass-through is driven by mouse events and
  a 100ms poll that only runs while the pointer is on the strip, so Ctrl
  pressed with the pointer already resting on a button takes up to a poll
  to take effect. Currently accepted, not fixed.
- Taskbar: halo-on-row-hover works, but keep in mind the independent
  problem it inherits: `ActiveWindowHalo` pins itself *directly beneath
  its target* (`HWND_TOP` silently no-ops for a background process), so a
  window that is minimized or fully covered shows nothing at all. The
  panel skips minimized rows for this reason; a covered one still shows
  nothing.
- Screencapture: capture larger than screen
- Move/resize windows: need mode that makes this easy
- Fix halo: when you activate window from taskbar, it doesn't get halo
- Fix halo: halo goes under explorer windows even when active app is in front (not always)
- Groups: Active window title should look very different than inactive
- Need a more unique icon...current icon looks like Google Gemini
- Alt+`: widen the tab allowlist beyond the five apps probed so far
  (`TabSwitching.h`). MDI children are still untouched and want a
  different mechanism entirely -- plain `EnumChildWindows` under an
  `MDIClient`, no UIA. Known gaps: Edge exposes only some of its tabs
  (docs/LIMITATIONS.md #18); Notepad's tab titles come through as
  accessibility labels ("top. Modified."), which wants stripping without
  hard-coding English suffixes.
- Alt+`: clicking a row in the tab panel does nothing -- the panel's row
  callbacks are keyed on HWND and a tab has none. Keyboard only for now
  (row action buttons and the Del/-/+ footer are correctly hidden for
  tab rows). Wants an index-keyed activation callback.
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
- Virtual desktops: remember which apps were on a desktop and offer to
  reload them.
- Virtual desktops: an app pinned to show on all desktops should keep
  that setting (persist it, not just apply it once).
- Virtual desktops: show an indicator of which desktop you're currently
  on.
- Virtual desktops: use visuals generally to make the whole concept
  easier to understand (exact treatment still unscoped).
- Do a full `docs/LIMITATIONS.md` pass and manual test matrix.
- Backlog, not yet scoped: paste history popup,
  Quick Access rename without renaming the file, radial start menu,
  consider a WinUI3 rewrite.

### Mouse sticky

Resistance at the edges the pointer keeps crossing by accident. One
mechanism -- the pointer slows or stops at a boundary until it is pushed
through deliberately -- with several places that want it. Not a taskbar
feature: the taskbar is only one of the edges.

- The edge between monitors, so a pointer aimed at something near the
  border does not shoot onto the next screen.
- The screen border itself, when reaching for snap targets or tools that
  live there.
- An auto-hidden taskbar, so passing near the bottom of the screen does
  not unhide it.

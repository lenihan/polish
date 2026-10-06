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
- Taskbar: closing an app's last window from the hover panel closes the
  panel, instead of leaving it on screen still listing the window that
  was just closed. The open panel was tracked purely by its *index* into
  the button list, and an index only means anything against the list it
  came from: losing the app's button shifts every later button down one,
  so the stored index then named a different app's button -- or pointed
  past the end of a shorter list, where the refresh check silently did
  nothing at all. It now also remembers which button that index *means*
  (AppUserModelID plus the taskbar it is on) and re-locates it after every
  read, closing the panel when the button is gone. A pinned app was always
  the easy half of this and still is: its button survives with no windows
  left, so the membership check finds the list empty and closes the panel
  itself.
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

- Easy move/resize mode: hold Win and the whole window becomes a move
  target, with the right button resizing by whichever corner quadrant it
  grabbed -- no title bar to find and no 7px border to hit. Space latches
  a keyboard session that outlives the hold (arrows move, Shift+arrows
  resize the active corner, Tab cycles the corner, Ctrl+arrows jump flush
  to the next snap target, Enter commits, Escape puts the window back
  exactly). What the design turns on:
  - The dim waits 250ms and any other key hands the whole hold back to
    Windows untouched, because Win is the busiest modifier on the
    keyboard. Win+L/D/E/arrow/digit never flash it.
  - The Win-up can never be swallowed (that bug is in AltTabHook's
    history: the OS is left believing the key is held, system-wide,
    surviving process exit), so the Start menu is stopped the other way
    -- an invisible Ctrl tap once per hold, the moment the mode first
    does anything visible.
  - That tap alone is not enough, and the gap was user-reported: the Win
    key *repeats* while held, at ~30/s, and every repeat key-down
    re-arms the shell's "standalone Win press" condition and discards the
    tap. So releasing Win after moving a window opened the Start menu on
    top of the window just moved. Measured against the bare OS with
    Polish stopped: Win-down, Ctrl tap, Win-up leaves Start shut, and the
    same sequence with a dozen repeat downs inserted before the up opens
    it every time. The repeats are now swallowed once the mode has
    committed to the hold -- safe, unlike the key-up, because the OS took
    the key as held from the first down (which is passed through) and
    nothing counts Win repeats. Answering each repeat with another Ctrl
    tap was the alternative and is the worse trade: ~30 injected
    keystrokes a second into whatever app is under the cursor.
  - Why no probe caught it: `keybd_event` sends one key-down and no
    repeats, so synthetic input cannot produce the condition at all. The
    first probe also closed the Start menu unconditionally as cleanup,
    which would have hidden it, and checked only for
    `StartMenuExperienceHost` as the foreground window when opening Start
    actually surfaces `SearchHost`. Three separate reasons a green probe
    meant nothing here.
  - Snapping is magnetic and never blocking: recomputed each frame from
    the raw pointer rect, so it attracts within the threshold and simply
    stops past it. Windows still overlap freely with no modifier.
  - The monitor edge is a hard clamp, so a window slammed at a shared
    edge parks flush instead of spilling over. Pushing >40px past it
    releases to the next monitor -- but only if the *pointer* has
    actually reached another monitor. Without that second condition a
    hard shove pushed the window off the side of the desktop, which a
    probe caught at x=-150 on a single-monitor machine.
  - Snapping works in visible-rect space, because two windows flush in
    raw `GetWindowRect` coordinates show a ~14px gap between the edges
    you can see. The inset is sampled once per grab and consumed inside
    that one drag, never stored -- which is what keeps it clear of
    `RectUtils.h`'s standing warning. Escape's rect is a raw
    `GetWindowRect` value replayed verbatim, exactly as that warning asks.
  - A Polish-driven drag fires an `EVENT_OBJECT_LOCATIONCHANGE` flood
    with no `MOVESIZESTART/END` around it, so `g_inMoveSizeLoop` cannot
    gate it; `g_moveModeMovingWindow` does, and the drop hands the final
    rect to restore-position sync by the front door.
  - Geometry is `windowtracking/MoveSnap.h`, Win32-free and unit-tested
    (18 cases): quadrants, snap at threshold +/-1, butting vs alignment,
    the clamp and its oversized-window case, min-size per corner. The
    visuals are `AltTabDimOverlay` and `AltTabHighlightBorder` reused
    as-is -- no new rendering code.
  - Two bugs the probes found and the tests could not: a debounce flag
    that stuck because Space's key-up took the session branch its
    key-down had not, killing every later Win+Space; and `SetCursorPos`
    not feeding a `WH_MOUSE_LL` hook at all, which made the first probe
    pass while exercising nothing.
  - A third doorway, for the mouse: the tray menu's "Move or resize a
    window". Both gestures start with the Win key, which left a mouse-only
    user with no way in at all.
    - Needed a public `MoveModeHook::BeginKeyboardSession`, because
      `keyboardSession_` was private and only ever set from inside the
      keyboard hook. The load-bearing part of it is `InstallMouseHook()`:
      that was only reached from the Win-down path, so without it
      "any mouse button commits the session" would have been dead code for
      a session started from the menu -- the mouse would have had no way to
      end what the mouse began. Verified with a probe that drives the menu
      command, moves with arrows, then commits with a real click and checks
      that later arrows are inert again.
    - It deliberately does *not* inject the Start-menu-suppressing Ctrl
      tap. There is no Win hold to suppress, and the tap would land in
      whatever app is under the cursor for nothing.
    - The target comes from the MRU list, for the same reason
      `ArrangeTargetMonitor` uses it: opening the tray menu takes the
      foreground for Polish's own message window, so `GetForegroundWindow`
      answers "us", and the cursor is on the tray icon rather than over
      anything movable. Neither signal the Win+Space latch relies on
      survives a trip through a menu. The grab point is the target's own
      centre for the same reason.
    - Minimized windows are skipped, unlike maximized ones: a maximized
      target is restored and then visibly follows the arrows, but
      restoring a window the user cannot see in order to move it is a
      surprise.

- Halo: drawn in the user's accent colour rather than a tone picked from
  the theme, with a hairline of the opposite brightness underneath it.
  The original design was white in dark mode and black in light mode,
  which sounds right and is not: what the glow must contrast with is
  whatever window sits *behind* the focused one, and that has nothing to
  do with the system theme. Reported with a screenshot -- an active black
  terminal on top of a white page, on a dark-mode desktop -- where the
  white glow was invisible against the white page, the one place it was
  needed. A first attempt added a one-DIP ring of the opposite tone,
  which was measurable but, at one DIP against a white page, read as an
  ordinary window border rather than a focus cue.
  - A saturated mid-luminance hue reads against both extremes where white
    and black each fail against one of them, and it matches what Alt+Tab
    already does, so the two focus cues finally agree.
  - The accent is pulled into a luminance band (60-185) before use, hue
    preserved by scaling all three channels, because an accent is allowed
    to be nearly white or nearly black -- which would put the glow
    straight back into the state this change exists to escape.
  - The hairline's tone comes from the accent's own luminance, not the
    theme: white under a dark accent, black under a light one. It gives
    the glow a crisp inner boundary and is the fallback for the one case
    the accent cannot cover alone, a backdrop of the accent's own colour.
    It sits strictly outside the target's edge; inside is the underlap
    (`kUnderlapDip`), and putting the opposite tone there would reopen
    the uneven-edge bug that constant was added to close.
  - `PremultipliedPixel` loses its pure-white/black shortcut and does a
    real multiply. Effectively free: the bands are the overwhelming
    majority of the pixels and compute one value per row or column before
    a fill_n/memcpy, so it runs per line, not per pixel.
  - The theme is no longer an input at all, so `isDark` is gone from the
    renderer and from the cache key, replaced by the accent colour --
    which means changing the accent in Settings takes effect without a
    restart.
  - `halo_math` gains `Luminance` and `ClampGlowLuminance`, unit-tested
    alongside the falloff and the outline (COLORREF's 0x00BBGGRR channel
    order, in-band passthrough, near-white pulled down, near-black pushed
    up, pure black becoming a grey, and hue survival).
  - Measured rather than eyeballed, since the eyeball was not available.
    Screen-captured a scanline across the edge against each extreme. Over
    a white backdrop the glow now reads at luminance 118 against 235
    across ~20px, where the old white glow was invisible and the old
    hairline was 2px; over a dark backdrop the hairline reads 179 against
    25. Both backgrounds now carry a strong cue, from opposite halves of
    the pair.

- Move/resize mode: moving a window no longer raises it. The whole point
  of dragging a background window from anywhere on it is to tidy it
  without disturbing what is in front, and several things along the way
  raised it anyway -- `ShowWindow(SW_RESTORE)` on a maximized target does
  by documentation, and a raise was measured during the grab handler on a
  window with five others above it. Rather than chase each one, the real
  window the target sat beneath is recorded at grab time and its place
  re-asserted after the restore, at the drop, and at session end. Insert
  relative to a named window, never `HWND_TOP`, which silently no-ops for
  a background process (the trap `PlaceHaloBehindTarget` already
  documents). Recorded by skipping Polish's own windows, since a dimmed
  target has its own dim overlay directly above it.
  - Three measurement traps cost most of the time here, all worth
    knowing. `msinfo32` re-orders itself while it loads, so it is useless
    as a z-order reference and produced two confident false "RAISED"
    verdicts. A WinForms window made from PowerShell does not pass
    `IsCandidateWindowShape` (logged `candidate=false`), so Polish
    ignores it entirely -- the probe silently grabbed VS Code instead and
    measured nothing, and the same thing makes such a window useless as a
    halo target. And counting *all* visible windows above the target
    wobbles with Polish's own dim overlays appearing and disappearing;
    counting only real candidate windows is the stable measure.

- Move/resize mode: rebuilt the gesture around zones on the left button
  alone. The right button was doing resize, which is genuinely awkward on
  a touchpad -- and a touchpad is where this gets used. Now an inch-wide
  band around the window's edge resizes and everything inside it moves,
  with the left button for both.
  - Eight resize zones rather than the old four quadrants, because an
    inch of band has room for the edges as well as the corners and
    dragging one edge is usually what is wanted. `Grip` grew from five
    values to nine, and the per-edge predicates had to stop being
    `left-or-else-right`: an edge grip drags one edge and must leave the
    other three alone, which the old `else` silently got wrong.
  - The band is an inch (`ResizeBorderPx`), clamped to 30% of the smaller
    dimension so it can never eat the move area, and 0 on a window too
    small for any sensible band -- which makes the whole window a move
    target, the safe way round, since a window can always be resized from
    the keyboard but an unmovable one is stuck.
  - `ZoneRect` is the inverse of `GripForPoint` and sits next to it
    deliberately: the map the UI draws and the hit-test that decides what
    a click does are the same geometry, and a map one pixel out of step
    with the band that actually responds would be a bug nobody would
    think to look for. A test walks every point of a window and asserts
    the two agree -- 57k assertions, and the cheapest kind to keep.
  - `MoveModeZoneOverlay` draws it: the hovered zone filled, the
    band/move boundary drawn so the layout can be read, and the window
    outlined. On a drag the map goes away and only the edges that grip
    actually moves are drawn, heavily -- which is the half that makes a
    move and a resize look different *while* they happen.
  - Its content depends on the window's size and grip, never its
    position, so a move drag repositions the overlay with
    `UpdateLayeredWindow(hdcSrc=null)` and never touches a pixel. Without
    that split a full-window bitmap would be cleared and refilled 60
    times a second for the length of every drag. The zone fill is also
    dropped during a resize, where the size changes every frame and the
    bars already carry the message.
  - The hook stops classifying the gesture entirely: `Grab` is gone, and
    it reports only where the press landed. Which zone that is needs the
    target's rect and DPI, which is the caller's business.

- Move/resize mode: drop a move at a screen edge to snap to half, a
  corner for a quarter, or the top to maximize, with a preview of where
  it will land.
  - Driven by the *pointer*, not the window, the way the native gesture
    is: the window is clamped to its monitor, so its edges would reach a
    screen edge long before the user meant anything by it.
  - Corners beat edges, or the quarter would be unreachable -- a pointer
    in the top-left corner is inside the top edge's zone too. The corner
    square is much larger than the edge strip (40 DIP against 6), since
    arriving at a corner precisely is harder and overshooting into one is
    cheap when nothing commits until the button comes up.
  - The bottom edge deliberately means nothing, as it does natively:
    claiming it would make dragging near the taskbar unpredictable.
  - Maximize uses `ShowWindow(SW_MAXIMIZE)` rather than a work-area-sized
    rect, so the window's own Restore button works afterwards and
    restore-position sync has something meaningful to write.
  - `RectForLayout` tiles exactly -- the right half starts where the left
    half ends -- which an odd-width work area is the test for.
  - This does fire at the shared edge between two monitors, where the
    user may have meant to cross. Native does the same and it recovers
    the same way: nothing commits until the drop, so carrying on past
    dismisses the preview and crosses.
  - Verified live on all six gestures with synthetic input: centre drag
    moves without resizing, the right band changes width alone, the
    bottom-left corner moves left and bottom alone, and the three layout
    drops land on exact halves, quarters and a real maximize.


- Build identity in the tray tooltip: version, the commit it was built
  from, a `+` if the tree was modified, and the build time. The version
  alone could not answer the question that kept coming up -- two binaries
  both saying 0.3.0, behaving differently, with nothing on screen to tell
  them apart. The same line goes into the log at startup, so a log file
  identifies its own binary.
  - Stamped by `cmake/BuildStamp.cmake` before *every build*, not only at
    configure time. A configure-time stamp goes stale on the next
    rebuild, which is exactly the case it exists for; that would have
    been a stamp that lies, which is worse than none.
  - Only the executables link the generated `BuildInfo.cpp`, never
    `polish_core`. The build time changes every build, so the file
    changes every build, so something must recompile -- this keeps that
    down to one tiny translation unit and a relink, and never disturbs
    the test binary.
  - `git status --untracked-files=no`: a stray scratch file says nothing
    about what went into the binary, and a stamp that cried "modified"
    over one would train you to ignore it.
  - Falls back to commit "unknown" outside a git checkout or with no git
    on PATH, so a source archive still builds.
  - `TrayIcon` takes the tooltip from its caller rather than owning it:
    what is worth saying there is build identity, and the tray class has
    no business knowing about that. It keeps the string because the icon
    is rebuilt from scratch when explorer restarts.
  - Both branches verified: the working tree stamps `6383594+` with
    `BuildIsModified() == true`, and a throwaway `git worktree` at the
    same commit stamps `6383594` with false -- same commit, different
    stamp, which is the whole point.


- Move/resize mode: fixed the drag-to-top maximize, which flashed and
  took only sometimes. Both symptoms, one approach to the top edge --
  measured, five layout transitions in a third of a second.
  - The trigger strip is ~12 physical pixels at 192 DPI and no hand holds
    a pointer that still, so the pointer crossed in and out of it
    repeatedly. Each crossing showed or hid a full-work-area preview, and
    showing it repainted a 2880x1824 bitmap: that was the flash.
  - `LayoutWithHysteresis` now asks the same question of a zone grown by
    `kLayoutReleaseDip` once a layout is engaged, so entering is easy and
    leaving is deliberate. A *different* answer from the enlarged zone is
    a switch rather than a release, so sliding along the top edge into a
    corner still trades maximize for a quarter. Measured again after:
    15 transitions across three identical approaches became 3, one each,
    with no oscillation at all.
  - `Hide` no longer throws the overlay's bitmap away. Content validity
    and visibility are separate now, so showing the same preview again is
    a window show, not a repaint -- which is what made each flicker
    expensive rather than merely visible.
  - The drop settles the layout against `GetCursorPos` at the moment of
    release rather than against the last drag message. Those differ:
    drag messages are coalesced, so the final pointer position before the
    button comes up may never arrive as a drag at all, and whether a
    release counted came down to which samples happened to land. Where
    the button came up is what the user is asserting, so that decides.
    Releasing 20px from the top edge used to be a coin toss and is now
    reliable.


- Move/resize mode: the snap preview now actually shows up. It was
  rendering correctly and arriving too late to see -- reported as the
  screen only going blue occasionally, and the maximize "working even
  when it isn't blue", which is exactly what that looks like from the
  outside: the drop is settled from the cursor at release, so it worked
  whether or not the preview had landed.
  - The preview was a MoveModeZoneOverlay, which rasterizes per-pixel
    alpha into a DIB. Right for the zone map -- different alphas in
    different places, only ever one window's size -- and wrong for this:
    a maximize preview is the whole work area, so every appearance meant
    filling 5.25 million pixels and pushing 21MB to the compositor,
    measured at 32ms for the first show.
  - `MoveModeLayoutPreview` replaces it with a flat
    SetLayeredWindowAttributes alpha over a solid fill, the same
    technique AltTabDimOverlay uses. The panel is one uniform colour, so
    per-pixel alpha was buying nothing. Measured at 0ms. The border is a
    lightened accent rather than a second alpha, since LWA_ALPHA applies
    one value to the whole window.
  - `kLayoutEdgeDip` 6 -> 24. Six DIPs is six physical pixels at 96 DPI;
    the pointer only reaches that at the very end of a throw at the edge.
    Erring generous is right: nothing commits until the button comes up,
    so an unwanted offer costs a shrug while a late one costs the whole
    point of a preview. `kLayoutReleaseDip` 32 -> 24 to keep escaping it
    from becoming a chore.
  - `ShowFill` and its isFill plumbing are gone from MoveModeZoneOverlay
    rather than left to rot.
  - Verified by sampling screen pixels during a held drag: two points at
    opposite ends of the work area read (2,2,2) while merely dimmed and
    (1,53,92) once the preview engages, which is the accent at the
    panel's alpha. The maximize still lands.
  - A measurement trap worth recording: the first run of that check
    reported the preview never drawing at all. The display layout had
    changed between sessions -- single 192 DPI screen to multi-monitor at
    96 -- and the sample points were no longer on any monitor, so they
    read pure black, which is indistinguishable from "nothing drew".
    Sample points have to be derived from the monitor's own work area at
    run time, never hard-coded.

- Move/resize mode: the maximize preview vanished at the moment you
  committed to it. Dragging toward the top showed the full-screen
  indicator; pushing all the way to the edge made it disappear, and the
  drop then often failed too.
  - Cause, measured: a low-level mouse hook reports the pointer
    *unclamped*. Shove at the top of the screen and keep pushing and the
    hook says y=-173 while GetCursorPos says y=0. `MonitorFromPoint` with
    `MONITOR_DEFAULTTONULL` answers "no monitor" for that point, so
    `LayoutForPointer`'s own on-this-monitor guard -- which is right, and
    exists for genuine multi-monitor cases -- correctly withdrew the
    offer at exactly the moment the user was pushing hardest for it.
  - Fixed where the monitor is chosen rather than by weakening the guard:
    `MONITOR_DEFAULTTONEAREST`, then `ClampPointToRect` to pull the point
    inside that monitor before asking. The drag path and the
    release-time recompute do it identically, so the preview and the drop
    can never disagree about which layout is pending.
  - Reproducing it needed *relative* mouse input. Normalized absolute
    injection cannot express a position off the screen -- 0 maps to the
    top edge exactly -- so every earlier probe drove y=0 at best and the
    bug was invisible to all of them. A real mouse produces relative
    deltas, which is why a hand found this in seconds and the harness
    never did. Probes that matter at a screen edge have to use
    `mouse_event` without MOUSEEVENTF_ABSOLUTE.
  - Verified: the preview stays lit through eight further shoves past the
    top edge (a far-screen pixel holds at the accent), and the maximize
    landed 11 times out of 12 -- the one failure was in a run where the
    probe itself had just been edited, and has not recurred since.

- Move/resize mode: after a snap the window's outline ring was left
  behind at the pre-snap position until the Win key came up. Only the
  half/quarter branch moved it, because that goes through
  `ApplyMoveModeRect`, which redraws the ring as part of its job;
  `ShowWindow(SW_MAXIMIZE)` does not, so a maximize never repositioned
  it. One `UpdateMoveModeOutline()` after the layout is applied, covering
  both branches -- a no-op repeat for the rect one, which the ring's own
  size cache absorbs.
  - Checked before assuming an animation race: DWM reports the maximized
    rect immediately at that point, identical at +250ms, so there is no
    lag to wait out and no timer needed.
  - Evidence is weaker than usual and worth saying so. The mechanism is
    plain in the code and the fix is one line, and the ring does appear
    at the maximized rect once it is applied (the screen edge reads pure
    accent, (0,120,212) across 5px, against (13,107,179) from the zone
    map alone with the fix disabled). But three attempts at a probe that
    detects the *stale* ring at its old location all failed for probe
    reasons, not product ones: the first sampled where the window had
    been before the drag rather than where it was at release; the second
    counted the zone map's hover fill, which legitimately covers the
    maximized window, as a ring -- it read identically with the fix on
    and off; the third died on PowerShell array and here-string quoting.
    A detector has to separate the ring (near-raw accent, alpha ~200)
    from that fill (same hue, alpha 70, around (44,76,102)), and scan
    where the window sat *at the moment of release*.

- Tile 2-way / 3-way / 4-way: Ctrl+Alt+2, Ctrl+Alt+3 and Ctrl+Alt+4 split
  the current monitor between that many *normal* windows,
  most-recently-used first. Two doorways on purpose -- the tray menu for
  the mouse, a hotkey for the keyboard -- and the menu prints the key
  beside each item, so the mouse way in is how anyone learns the keyboard
  way in.
  - Replaced an earlier "tile every window into a ceil(sqrt(n)) grid" plus
    a cascade. The grid answered a question nobody asks: past about four
    slots every window is too small to work in, so the result gets looked
    at once and undone. Naming each command for its count is the whole
    fix -- the digit *is* the number of windows. Cascade went with it,
    since a stack of overlapping windows is what Stacks does properly.
  - The split axis is the monitor's longer dimension, so two windows on a
    16:9 monitor get a usable shape each instead of two letterboxes, and
    turning the monitor on its side flips the split with it. 4-way is
    always a 2x2, in both orientations.
    - Deliberately *not* "slice in two, then slice each half along its own
      long axis", which is tidier and gives quarters on an ordinary
      monitor -- but on a 3840x1080 ultrawide each half is still
      landscape, so it would slice the same way again and produce four
      columns. Four columns may be the better layout there; it is not what
      the command is called, and a shape that depends on the aspect ratio
      in a way nobody can predict is worse than one that is merely
      suboptimal. A test pins the ultrawide case.
  - Only the N most recent windows are touched; anything older stays
    exactly where it is. Rearranging the whole desktop as a side effect of
    "let me see these two" is how a convenience becomes something people
    stop using.
  - Pressing the same command again **reverses** the order, and a third
    press returns to MRU -- a toggle, not three states. The first guess at
    which window belongs on the left is wrong about half the time, and
    pressing the key again is cheaper than dragging.
    - The toggle's reset key is the subtle part, and it is why
      `ArrangeSelection` is a separate pure module with its own tests. It
      keys on the *sorted set* of chosen windows. Keying on MRU order
      would reset the toggle almost every time, because focusing either
      window between two presses reorders the list -- the reversal would
      look broken in exactly the situation it is for. Keying on the
      *chosen* order is worse still: reversing changes it, so the toggle
      would see its own effect as a change and never reverse twice.
    - State is per kind. 2-way and 3-way are different commands, and using
      one must not leave another halfway through its own toggle.
  - A command with too few windows is greyed out in the menu **with the
    reason in the item's own label** ("Tile 4-way (needs 4 windows)"). A
    standard Win32 popup menu has no per-item tooltip, and this app
    deliberately has no owner-drawn menus (`util/DarkMode.cpp` -- the dark
    theme comes from `SetWindowTheme`, which owner-draw would opt out of),
    so the label is the only channel there is. The hotkey stays in its own
    tab-separated column after the parenthetical, so the three items still
    line up. The hotkey path refuses and logs independently, since it is
    reachable while the menu item is greyed.
  - Normal means neither minimized nor maximized
    (`IsWindowInNormalState`, the same predicate restore-position sync
    uses). Maximized is a deliberate exclusion, not a technical limit:
    the first version restored a maximized window into a tile slot,
    which quietly undid a state the user had explicitly asked for, on a
    window they may not have been thinking about at all. A maximized
    window is already arranged; tile and cascade work around it. That
    also removed the SW_RESTORE from the apply path, since nothing
    maximized can reach it any more.
  - `windowtracking/WindowLayout.h` holds the geometry and
    `windowtracking/ArrangeSelection.h` the policy, both pure and tested
    (34 cases). Both live outside `main.cpp` for a structural reason
    rather than a stylistic one: `polish_tests` links `polish_core` only,
    so anything in `main.cpp` cannot be unit tested at all.
  - 2-way and 3-way are one function, `SliceAlongLongAxis(work, n)` --
    3-way is not a special case, it is n = 3. Every boundary is computed
    once as `left + width * i / n` and shared between the slices either
    side of it, so no rounding can leave a seam and the last slice lands
    exactly on the far edge. Tested with a width that does not divide by
    three (1001), which is where a naive `width/3` loses a pixel and shows
    a strip of desktop between two windows.
  - The MRU list only knows windows focused since Polish started, so the
    candidate set is MRU first then an EnumWindows Z-order tail -- the
    same shape RebuildAltTabCandidates uses, and arranging is exactly
    where the windows you have not touched this session would be most
    obviously missing.
  - Ctrl+Alt plus the digit, rather than Win+Alt, which would have matched
    the group hotkey. Probed first: Win+Alt+T, Win+Shift+T, Win+Shift+C
    and Win+Ctrl+C were all already claimed on this machine, and a pair
    with mismatched modifiers is worse than a pair that is not Win-based.
    All three of Ctrl+Alt+2/3/4 were confirmed registering here.
    Registration failure is logged per command, because the group hotkey's
    history is that a reasonable default was already taken -- and it still
    is: Win+Alt+G fails on this machine with error 1409.
  - The registry value names are new (`ArrangeTwoWayHotkey*` and friends)
    with no fallback to the old `ArrangeTile*`/`ArrangeCascade*`, on
    purpose: those hold 'T' and 'C', which are the wrong keys for these
    commands, so inheriting them would leave a user on a hotkey that no
    longer matches anything the menu says.
  - A bug worth remembering: the first version called
    `SyncRestorePlacementNow` straight after an async `SetWindowPos`.
    That reads the window's *current* rect -- still the old one -- and
    writes it into `rcNormalPosition` via `SetWindowPlacement`, which
    repositions the window right back. Every arrangement applied and
    silently undid itself, while the slot assignments in the log looked
    perfectly correct. The sync happens on a 250ms timer now, once the
    moves have landed.
  - An app can refuse to be as small as its slot (`WM_GETMINMAXINFO`) and
    nothing can overrule that. Measured: a four-way tile gave one window
    a 912px slot and it came back 1286 tall, 374px past the bottom of the
    screen. It cannot be made to fit, so it is slid back on screen
    instead -- moved, never resized -- and overlaps its neighbour rather
    than disappearing off the edge.
  - Verified with a probe that snapshots every window's placement first
    and restores it afterwards, since the command moves the real desktop
    and a test that leaves it scattered is not one worth running twice.


## v1.0 release -- Microsoft Store

The route is settled and should not be re-litigated: v1.0 ships through the
Store's **EXE/MSI route** (Store Policy 10.2.9, available since June 2021),
where the Store lists the app and launches an installer we host and sign
ourselves. MSIX was rejected because **MSIX and UIAccess are mutually
exclusive**, from three independent directions: `C:\Program Files\WindowsApps`
was deliberately removed from the UIAccess secure-directory list as a
security mitigation (Project Zero, Feb 2026); Windows refuses UIAccess to
packaged desktop processes outright ("UI Access is not supported for Desktop
AppX processes", microsoft/WindowsAppSDK#1669, open since 2021); and
AutoHotkey's Store edition ships with UIAccess disabled for exactly this
reason. Going MSIX would mean giving up `polish_uia.exe` and with it the
Start-open case that #24 documents -- the shield would lose the z-band fight
again whenever the Start menu or Search is open. The EXE route keeps every
technique the app currently relies on.

What that route demands, and what shapes most of the work below: `.exe` or
`.msi` only, **every PE file signed** with a chain to a Microsoft Trusted
Root, a **versioned HTTPS URL that may never change** once submitted,
**silent install** (a UAC prompt is allowed), and a standalone offline
installer rather than a downloader stub.

Decisions taken: everything that works today ships, Groups included; free;
individual Partner Center account; the Store is the only user-facing
channel; GPL-3.0; **SignPath Foundation** for signing, which is free for
open-source projects -- at the cost of the binary's publisher reading
"SignPath Foundation" rather than a personal name, and manual approval per
release.

**Sequencing matters more than the individual steps.** Two external
processes gate everything and have unknown or multi-day latency, so they
start first: SignPath's approval, and Partner Center's identity
verification. SignPath also requires the project to be *already publicly
released in the form to be signed*, which means an unsigned v1.0.0 GitHub
release has to exist before the signing application can even go in.

- **Licence and repo hygiene.** A `LICENSE` file (GPL-3.0) -- there is none
  today, and SignPath requires an OSI-approved one. MFA on the GitHub
  account, also a SignPath requirement. `kAboutUrl` (main.cpp) points at a
  personal LinkedIn profile and should point at the project's own page.
  `out.png` and `HANDOFF.md` sit in the repo root, and README still says
  "No installer", "isn't code-signed yet" and "both features" when there
  are five.
- **A version number.** There is none anywhere: `project(polish)` carries no
  VERSION, and neither `.rc` has a VERSIONINFO block, so Explorer's Details
  tab is blank and signing tooling has nothing to read. One source of truth
  in CMake, feeding both resource scripts and the manifests'
  `assemblyIdentity`.
- **First-run consent, for Store Policy 10.2.8.** The policy requires user
  consent before changing the Windows experience and names "undocumented or
  unsupported APIs in unsupported ways" among unsupported methods --
  replacing Alt+Tab, shielding the taskbar strip and installing global hooks
  is squarely that, and this is the likeliest certification failure. A
  first-run modal names what Polish changes and takes an explicit opt-in;
  nothing hooks or shields until it is answered. `GroupHotkeyDialog` is the
  pattern to copy, and the per-feature switches it needs already exist and
  already restore native behaviour when off. The uxtheme ordinals 135/136 in
  `DarkMode.cpp` are the only undocumented calls left in shipping code
  (cosmetic, already null-checked) and are the clearest target to drop;
  `GetWindowBand` is not actually called anywhere.
- **Clean uninstall, for Store Policy 10.2.7.** Files, `HKCU\Software\Polish`,
  the `Run` value, and `%TEMP%\polish.log`.
  `tools/uiaccess/Uninstall-PolishUiAccess.ps1` already does the file half.
- **An installer.** WiX/MSI rather than Inno Setup, because `msiexec /qn`
  satisfies the silent-install requirement for free and MSI gives clean
  uninstall and an Add-or-Remove-Programs entry without hand-rolling them.
  It installs to `%ProgramFiles%\Polish` -- a UIAccess requirement, not a
  preference -- and ships the UIAccess build only; `polish.exe` stays a
  development target. It handles no certificates at all: a real signature
  makes `Install-PolishUiAccess.ps1`'s self-signed root-certificate
  injection unnecessary, which removes the most security-sensitive thing
  this project currently asks of a machine. Start-at-login keeps working
  unchanged, since an unpackaged app may still use the `Run` key -- the one
  thing MSIX would also have broken.
- **CI.** There is no `.github/` at all; releases are a manual
  `cmake --build`. A workflow that builds Release, runs the tests, produces
  the installer and submits it for signing is what makes a release
  repeatable. The static CRT is already configured, so there is no
  redistributable to carry.
- **Partner Center and the listing.** Reserve the name early: "Polish" is a
  common English word *and* a nationality, and Store naming rules forbid
  descriptive keywords, so have alternatives ready rather than discovering
  the problem at submission. A privacy policy URL is mandatory for Win32
  products (policy 10.5.1) whatever the app collects -- GitHub Pages from
  this repo, alongside a support page. IARC rating, screenshots, and a full
  icon/tile asset set, where only a single `.ico` exists today. The listing
  and submission notes should state the global hooks, the cross-process
  `SetParent` behind Groups and the UIA read of the taskbar's own button
  class plainly (policies 10.1.1/10.1.4) rather than leave a certification
  tester to discover them.
- **Verify on a clean machine, not this one.** The dev box has a dev
  certificate in its root store, a stale `%ProgramFiles%\Polish` and an
  existing `HKCU\Software\Polish`, every one of which would mask a real
  installer bug. Windows Sandbox or a spare machine; check silent install,
  first-run consent, the Start-open shield case, a run with every feature
  switched off, and that uninstall leaves nothing behind.

## Left to do
- Regular app presence: Polish currently lives only in the tray, which is
  hard to tell is running and often hidden in the overflow. Give it a real
  window and a taskbar button.
  - A small main window carrying the feature toggles as real controls (the
    tray menu's checkboxes, plus Tile/Stack/Move commands), so it is
    discoverable and the state is visible. Start-at-login starts it
    minimized.
  - Keep the tray icon (optional): it is the right place for "running in
    the background" and costs nothing.
  - Taskbar-button gestures are the risky part. A click on a taskbar button
    means "activate this window", so "click = Alt+Tab" has to be done by
    intercepting the activation/restore and opening the switcher instead,
    which can flicker. Right-click is the shell's jump list, not a menu we
    own: expose the full command set as jump-list tasks
    (ICustomDestinationList, needs an AppUserModelID) rather than trying to
    replace the shell menu. Prototype the click behaviour before committing
    to it.
- Virtual Monitor: From a single monitor, split it into 2+ monitors with custom scaling.
  Mouse should "stick" inside a monitor. Alt+tab only shows apps running on that monitor.
  Can have different resolutions for each monitor. OS should think it is actually multiple
  monitors. Allows you to use ultra wide as two normal monitors. Maximize app fills virtual
  monitor, not entire wide monitor.
- Remove Groups: Too hacky. Does not support UWP.
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
- Move/resize mode: the "slick UI that flips windows around with
  move/resize controls" half of the original idea is not built -- today
  the affordance is the dim plus an outline on the window under the
  pointer, and nothing is drawn on the window itself.
- Move/resize mode: no snap-target preview. The window itself moving is
  the only feedback that a snap is about to take.
- Move/resize mode: never tried on a real multi-monitor or mixed-DPI
  desktop. The clamp release and the snap-edge resample on crossing are
  both written but only exercised single-monitor.
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

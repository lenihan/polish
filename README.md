# Polish

Add fit and finish to Windows 11 — a native, tray-resident utility that
smooths over a set of long-standing rough edges in window management.
Most of it improves a gesture you already use; the one genuinely new
gesture, holding Win to move or resize a window from anywhere on it,
exists because the thing it replaces — grabbing a title bar — is the
affordance modern apps took away.

## What it does

**Restore remembers Snap position.** Windows' native Snap (drag-to-edge,
Win+Arrow, Snap Layouts) doesn't update a window's "restore" position.
Snap a window to quarter-size, click its native Maximize button, then
click Restore, and Windows puts it back wherever it was floating
*before* the snap — not at the quarter size you just set up. Polish
fixes this in the background: whenever a window settles into a new
position (including right after Polish starts, for a window that was
already snapped before it launched), Polish syncs that position into the
window's restore target, so the native Maximize and Restore buttons
round-trip through your most recent snap correctly. No new gesture,
click, or hotkey — it just fixes what's already there.

**Alt+Tab skips minimized windows.** Windows' Alt+Tab cycles through
minimized windows right alongside open ones, which gets in the way once
you've minimized a few things to get them out of sight. Polish replaces
Alt+Tab with its own: hold Alt and press Tab as usual, and only
non-minimized windows are offered. The window you're about to switch to
is shown in its own real screen position — with a soft accent-colored
glow around it and everything else dimmed — rather than a thumbnail you
have to match up. A few things worth knowing:

- **Shift+Tab** cycles backward, same as native Alt+Tab.
- **Escape** cancels without switching.
- **Clicking** during a session both ends it (landing on whatever was
  highlighted) *and* still reaches whatever's under the cursor normally
  — so you can e.g. click a window's minimize button mid-cycle and it
  minimizes, while Alt+Tab still lands wherever you'd highlighted.
- **Ctrl+Alt+Tab** is a deliberate escape hatch: it bypasses Polish
  entirely and hands off to Windows' own native Alt+Tab, in case you
  ever want the original behavior back for one switch.
- The switcher opens whenever there's any window at all to show, even
  when there's nothing to switch *to*: with a single window open it still
  gives you that window's minimize/maximize/close buttons, and with none
  open but some minimized it's how you get one of them back. Only a
  desktop with no windows whatsoever -- none active, none minimized --
  leaves Polish out of the way, handing off to native Alt+Tab as normal.

**Alt+\` switches between the tabs of the window you're in.** Hold Alt and
press the backtick key (the one above Tab) to get the same switcher, but
listing the tabs inside the current window rather than windows -- the
files open in VS Code, the tabs in a File Explorer or Terminal window.
Windows has no equivalent. It reuses everything above (Shift, Escape, the
list panel), and the panel heading shows the app's name instead of
"Active". A few things worth knowing:

- **Most-recently-used order, so a double-tap toggles.** Like Alt+Tab, the
  list is ordered by how recently you used each tab, not by their position
  on screen -- so Alt+\` twice bounces between the last two tabs you were
  working in. Polish learns that order by noticing which tab is frontmost
  each time you use the switcher, so it is right from the second use of a
  window onward rather than the first.
- **It works per app, and only where tabs can be read at all.** Tabs
  aren't operating-system objects the way windows are -- they exist only
  inside each app's own interface, and the only way in is UI Automation,
  which every app answers differently. Polish therefore ships a list of
  apps it knows how to read: **VS Code, File Explorer, Windows Terminal,
  Notepad and Edge**. In any other app Alt+\` does nothing at all.
- **That list is deliberately conservative.** Several apps -- Outlook and
  OneNote among them -- expose only their *ribbon* tabs (Home, Insert,
  View) and no document tabs whatsoever. Offering those as things to
  switch to would be worse than doing nothing, so an app is only listed
  once its real tabs have been confirmed.
- **Mid-hold switching.** With Alt still held, press Tab during an Alt+\`
  session to widen back out to every window. The panel stays up.
- **Nothing to switch to does nothing.** In a window with a single tab,
  Alt+\` simply does nothing -- unlike Alt+Tab there's no native behavior
  to fall back on.
- The tray menu's Alt+Tab checkbox controls both.

**A soft halo around the active window.** A quick glow in your Windows
accent colour around whichever window currently has focus, fading out
over about a quarter inch on all four sides. Windows 11's own focus cues (a subtly different title bar, the
DWM shadow) are easy to miss on a busy desktop; the halo makes it
obvious at a glance which window you're typing into. It doesn't appear
on a maximized or full-screen window (there's no room outside their
edges, and it's already unambiguous which one is focused) or during an
Alt+Tab session.

- **It stays visible on any background.** The accent colour is the point:
  what the glow has to stand out against is whatever window happens to be
  *behind* the focused one, which has nothing to do with whether you run
  Windows in dark or light mode. A white glow disappears over a white
  page even on a dark-mode desktop — which is exactly where a
  theme-coloured glow failed. A saturated accent reads against a white
  page and a black terminal alike. Underneath it, hard against the
  window's edge, sits a hairline of the opposite brightness to your
  accent, which covers the one case the accent can't: a background that
  happens to be the accent's own colour. Same two-tone trick as the
  copy/paste bullseye, for the same reason. Change your accent colour in
  Settings and the halo follows immediately.

**Hold Win to move or resize any window, from anywhere on it.** Apps
increasingly draw their own UI into the title bar, or have no title bar
at all, so the strip you can grab to move a window has quietly shrunk to
nothing — and the resize border is about seven pixels wide. Hold the Win
key: after a moment every window dims, and the one under the pointer
shows its own map — an inch-wide band around the edge that resizes, and
everything inside it that moves. Drag with the **left button**, and where
you pressed decides what happens. Release Win to leave. There is nothing
to aim at in either gesture, and only one button to use.

- **The map shows what a click will do before you click.** The zone under
  the pointer is filled in, so you can see you're about to resize the
  right edge rather than move the window. Once you're dragging, the map
  disappears and only the edges actually moving stay lit — so a move and
  a resize look different while they're happening, not just before.
- **The band is an inch wide**, which is the point: the native 7px border
  is what this feature exists to replace, so a band you had to aim at
  would bring the problem back. On a window too small for a full inch it
  shrinks to keep a usable move area in the middle.
- **Drop at a screen edge to snap.** Drag a window to the left or right
  edge for half the screen, into a corner for a quarter, or to the top to
  maximize. A preview shows exactly where it will land, and nothing
  commits until you let go — so carrying on past the edge just dismisses
  it, which is also how you drag between monitors.
- **Windows snap to each other, but are never blocked.** Edges are drawn
  magnetically to the monitor's work area and to the visible edges of
  other windows, so you can butt two windows flush or line them up
  without aiming. Push a little further and the window slides straight
  past: overlapping needs no modifier, no override, nothing held down.
- **The monitor edge is a wall.** Slam a window at the boundary between
  two screens and it parks flush against it instead of spilling onto the
  next one. Keep pushing well past the edge and it crosses — no modifier
  for that either. On a single monitor it simply cannot leave the screen.
- **Without the mouse: Win+Space.** That latches onto the window under
  the pointer (or the active one) and keeps the session open after you
  let go of Win. Arrows move it, snapping as they go; **Shift+Arrow**
  resizes the active zone and **Tab** walks that zone around the eight
  edges and corners; **Ctrl+Arrow** jumps the window flush to the next
  snap target in that direction. **Enter** keeps it, **Escape** puts it
  back exactly where it started, and a click anywhere keeps it and ends
  the session.
- **Moving a window never raises it**, so you can tidy something in the
  background without bringing it to the front.
- **Normal Win shortcuts are untouched.** Pressing any other key during
  the hold hands the whole thing back to Windows, and the dim waits a
  quarter-second before appearing, so Win+L, Win+D, Win+E, Win+Arrow and
  Win+<digit> behave exactly as they always did and never flash anything.
  Tapping Win on its own still opens the Start menu; holding it and then
  letting go without doing anything does not.
- **Dragging a maximized window restores it under your cursor**, at the
  same relative spot you grabbed, rather than jumping to wherever it last
  floated.

**A bullseye for copy and paste.** Windows gives no feedback that a copy
took, or where a paste landed. Now a large, soft ring plays on top of
every window for a third of a second: on a copy it fades in and collapses
onto the spot you copied from; on a paste (Ctrl+V or Shift+Insert) it
does the reverse, bursting out of the spot the text lands. Click-through,
and silent during an Alt+Tab session or a full-screen game.

- **It aims at what you actually selected.** On a copy, the ring centres
  on the middle of the selected text, however many lines it spans.
  On a paste, where nothing is selected, it aims at the text caret
  instead -- including in apps that draw their own caret rather than
  using Windows', like browsers and Explorer's address bar. Failing both,
  it falls back to the mouse pointer, then the middle of the window.
- **It stays visible on any background.** The ring is drawn in two tones,
  a light core and a dark edge (or the reverse, following your theme), so
  one of the two always contrasts with whatever is behind it -- the same
  trick that keeps a mouse cursor visible everywhere. A single-coloured
  ring disappeared against a white document.

**Hovering the tray icon says which build you're running.** Not just the
version — the commit it was built from, whether the working tree had
uncommitted changes in it, and when it was built:

```
Polish 0.3.0 - 6383594+ - 2026-10-03 11:27
Add fit and finish to Windows
```

The trailing `+` marks a build made from a modified tree, the way most
git prompts mark one. This exists because the version on its own cannot
answer the question that actually comes up: two binaries both say 0.3.0,
behave differently, and nothing on screen says which is which. The same
line is written to the log at startup, so a log file identifies its own
binary.

**Tile or cascade everything on screen, in one go.** The gestures above
act on one window; these act on all of them. **Ctrl+Alt+T** tiles every
non-minimized window on the current monitor into a roughly square grid —
two windows become a left/right split, four become the corners, and an
odd count puts the leftovers in a full-width row rather than leaving a
hole. **Ctrl+Alt+C** cascades them instead, all the same size and stepped
down and right so every title bar stays readable; a long enough stack
restarts from the top-left rather than walking off the screen.

Both are also in the tray menu, which shows the key beside each one —
so the mouse way in teaches the keyboard way in. Windows are taken
most-recently-used first, so the one you were just in gets the first
slot.

**Only normal windows take part.** A minimized window stays minimized,
and a maximized one stays maximized and keeps its place — tiling works
around it rather than pulling it down into a slot. A window you
maximized is already arranged, and quietly undoing that is not what
"tile the others" should mean. Elevated windows are left alone too,
because Windows won't let an unelevated app move them (see
[`docs/LIMITATIONS.md`](docs/LIMITATIONS.md)).

One honest limit: an app can refuse to be as small as its slot, and
nothing here can overrule that. Such a window keeps the size it insists
on and is slid back onto the screen rather than left hanging off the
edge, so it overlaps its neighbour instead of disappearing.

If `Ctrl+Alt+T` or `Ctrl+Alt+C` is already taken on your machine, Polish
says so in its log at startup and the key simply does nothing. Change it
under `HKCU\Software\Polish` (`ArrangeTileHotkeyVirtualKey` and friends)
— there's no picker for these two yet.

Every feature can be turned off independently from the tray icon's
right-click menu, which also has a **Start with Windows** option and
Exit.

## Install and run

Download **`Polish-0.3.0-x64.msi`** from the
[Releases page](https://github.com/lenihan/polish/releases) and run it.

1. Your browser may warn that the file isn't commonly downloaded, and
   Windows will show a blue **"Windows protected your PC"** screen. Polish
   isn't code-signed yet, so Windows has nothing to identify the publisher
   by. Click **More info**, then **Run anyway**. (This goes away once the
   signed build lands -- see [`PLAN.md`](PLAN.md).)
2. Accept the admin prompt. Polish installs to
   `C:\Program Files\Polish`, which is where the signed build will need
   to live later.
3. Look for its icon in the system tray -- including the "^" overflow
   area, since Windows 11 hides newly-added tray icons there by default.
   Right-click it for the settings menu, including **Start with Windows**.

Every feature can be switched off independently from that menu, and
switching one off restores Windows' own behaviour completely.

To remove it: **Settings → Apps → Installed apps → Polish → Uninstall**,
which also takes its settings and startup entry with it.

**Requirements.** Windows 11 on x64. The taskbar features read the Windows
11 taskbar specifically; on Windows 10 those stand down and the rest still
works. An ARM64 machine runs the x64 build under emulation.

**Known gap in this build.** While the Start menu or Search is open,
Windows lifts the taskbar above Polish's overlay and its native thumbnail
flyout reappears over Polish's own window list. The fix needs a code-signed
build, which is the next milestone.

See [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) for known, permanent
gaps (e.g. elevated windows) and [`PLAN.md`](PLAN.md) for build progress
and history.

## Building from source

### Prerequisites

- Windows 11
- Visual Studio 2022 (17.8+) with the "Desktop development with C++"
  workload, and CMake 3.28+

Don't have those? Run `setup.ps1` (below) to install/upgrade them
automatically via `winget`.

### Setup

```powershell
git clone https://github.com/lenihan/polish.git
cd polish
.\setup.ps1
```

`setup.ps1` detects your existing Visual Studio/CMake install and only
installs or upgrades what's missing — safe to re-run any time.

### Build

Uses [CMake presets](CMakePresets.json), so the CLI stays short. Configure
once — the `default` configure preset generates a multi-config Visual
Studio build tree, so both Debug and Release build from it without
reconfiguring in between:

```powershell
cmake --preset default
```

| Configuration | Build | Run | Test |
| --- | --- | --- | --- |
| Release | `cmake --build --preset default` | `.\build\Release\polish.exe` | `ctest --preset default` |
| Debug | `cmake --build --preset debug` | `.\build\Debug\polish.exe` | `ctest --preset debug` |

Use Debug while developing (assertions, easier debugging in Visual
Studio/WinDbg); use Release to check real-world behavior and performance.

### Test

```powershell
ctest --preset default
```

Swap in `ctest --preset debug` to run against the Debug build instead.

## Project layout

See the `src/` tree for module boundaries (window tracking, Alt+Tab,
tray, settings) — each directory groups one concern.
`docs/LIMITATIONS.md` documents known, permanent gaps (e.g. elevated
windows).

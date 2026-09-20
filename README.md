# Polish

Add fit and finish to Windows 11 — a native, tray-resident utility that
fixes two long-standing rough edges in window management, with no new
gestures to learn.

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

**A soft halo around the active window.** A quick, theme-aware glow —
white in dark mode, black in light mode — around whichever window
currently has focus, fading out over about a quarter inch on all four
sides. Windows 11's own focus cues (a subtly different title bar, the
DWM shadow) are easy to miss on a busy desktop; the halo makes it
obvious at a glance which window you're typing into. It doesn't appear
on a maximized or full-screen window (there's no room outside their
edges, and it's already unambiguous which one is focused) or during an
Alt+Tab session.

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

All four features can be turned off independently from the tray icon's
right-click menu, which also has a **Start with Windows** option and
Exit.

## Install and run

No installer — Polish is a single `.exe`.

1. Download the latest `polish.exe` from the
   [Releases page](https://github.com/lenihan/polish/releases).
2. Run it. Windows SmartScreen will likely warn that it's from an
   unrecognized publisher (Polish isn't code-signed yet) — click
   **More info**, then **Run anyway**.
3. Look for its icon in the system tray (including the "^" overflow area
   — Windows 11 hides newly-added tray icons there by default).
   Right-click it for the settings menu, including **Start with
   Windows** if you want it running every login.

That's it — both features work automatically from there. Right-click the
tray icon → Exit to quit.

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

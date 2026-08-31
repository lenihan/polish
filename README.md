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
- If there are fewer than two non-minimized windows to switch between,
  Polish gets out of the way and native Alt+Tab runs as normal.

Both features can be turned off independently from the tray icon's
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

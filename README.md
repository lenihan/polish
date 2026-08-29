# Polish

A high-performance native Windows 11 utility that fixes a long-standing
gap in Windows' own window snapping.

**Windows' native Snap (drag-to-edge, Win+Arrow, Snap Layouts) doesn't
update a window's "restore" position.** Snap a window to quarter-size,
click its native Maximize button, then click Restore, and Windows puts
it back wherever it was floating *before* the snap — not at the quarter
size you just set up. Polish fixes this in the background: whenever a
window settles into a new position (including right after Polish starts,
for a window that was already snapped before it launched), Polish syncs
that position into the window's restore target, so the native Maximize
and Restore buttons round-trip through your most recent snap correctly.
No new gesture, click, or hotkey to learn — it just fixes what's already
there.

The app runs from the system tray — right-click the tray icon for an
Exit option. (Windows 11 hides newly-added tray icons in the "^" overflow
area by default; look there if you don't see it in the main tray strip.)

See [`PLAN.md`](PLAN.md) for build progress.

## Prerequisites

- Windows 11
- Visual Studio 2022 (17.8+) with the "Desktop development with C++"
  workload, and CMake 3.28+

Don't have those? Run `setup.ps1` (below) to install/upgrade them
automatically via `winget`.

## Setup

```powershell
git clone <repo-url> polish
cd polish
.\setup.ps1
```

`setup.ps1` detects your existing Visual Studio/CMake install and only
installs or upgrades what's missing — safe to re-run any time.

## Build

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

## Run

```powershell
.\build\Release\polish.exe
```

The app runs tray-resident (no visible window) once started. Look for its
icon in the system tray (including the "^" overflow area); right-click
for an Exit option. Restore-position sync then runs automatically in the
background for every window you focus — check `%TEMP%\polish.log` if you
want to see it working.

## Test

```powershell
ctest --preset default
```

(Reports "No tests were found" until Phase 1 adds the `tests/` target —
that's expected, not a failure.) Swap in `ctest --preset debug` to run
against the Debug build instead.

## Project layout

See the `src/` tree for module boundaries (window tracking, tray) — each
directory groups one concern. `docs/LIMITATIONS.md` documents known,
permanent gaps (e.g. elevated windows).

# Polish

A high-performance native Windows 11 utility. Its first feature adds a
"toggle size" button to the title bar of windows across the system —
clicking it snaps a window back and forth between its current size/position
and whatever it was immediately before (e.g. toggle between a half-screen
snap and a quarter-screen snap). A global hotkey provides the same toggle
for windows where a visible button can't be placed (see
[Known limitations](docs/LIMITATIONS.md)).

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

Uses [CMake presets](CMakePresets.json), so the CLI stays short:

```powershell
cmake --preset default
cmake --build --preset default
```

`cmake --build --preset debug` builds a debug configuration instead
(reconfigure isn't needed again — `default` is a multi-config Visual
Studio preset covering both).

## Run

```powershell
.\build\Release\polish.exe
```

The app runs tray-resident (no visible window) once started. Look for its
icon in the system tray; right-click for options. Default global toggle
hotkey: **Win+Alt+T**.

## Test

```powershell
ctest --preset default
```

(Reports "No tests were found" until Phase 1 adds the `tests/` target —
that's expected, not a failure.)

## Project layout

See the `src/` tree for module boundaries (window tracking, overlay
rendering, hotkey handling, tray/settings) — each directory groups one
concern. `docs/LIMITATIONS.md` documents known, permanent gaps (e.g.
custom-drawn title bars, elevated windows).

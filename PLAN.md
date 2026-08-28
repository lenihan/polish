# Polish — Build Plan

Living checkbox tracker for the Windows 11 title-bar toggle utility. Check
items off in the same commit that completes them. Design rationale and
full technical detail live in the design doc this was generated from; this
file is the at-a-glance progress tracker.

See [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) for known, permanent
limitations (not bugs to fix).

## Phase 0 — Scaffolding

- [x] `README.md`
- [x] `PLAN.md` (this file)
- [x] `setup.ps1` bootstrap script (detect/install VS2022 + C++ workload, CMake via winget)
- [x] `CMakeLists.txt` (C++23, `WIN32_EXECUTABLE`, `UNICODE`, `_WIN32_WINNT=0x0A00`)
- [x] `app.manifest` (Per-Monitor-V2 DPI awareness, `asInvoker`, Win10/11 `supportedOS` GUIDs) — note: the `dpiAwareness` element must use the `http://schemas.microsoft.com/SMI/2016/WindowsSettings` namespace, not 2017 (that's for `longPathAware`); using 2017 fails activation silently-ish (app still launches, but falls back to DPI-unaware — verify with `GetProcessDpiAwareness`, not just "did it start")
- [x] `resources/app.rc` (`RT_MANIFEST` embed; icon deferred to Phase 3 tray work)
- [x] `src/main.cpp` skeleton: `WinMain`, single-instance mutex, DPI-awareness verification, message loop
- [x] `docs/LIMITATIONS.md` stub

Verified end-to-end: `cmake -B build -S .` + `cmake --build build --config Release` succeeds;
built exe launches, is confirmed `PROCESS_PER_MONITOR_DPI_AWARE` via `GetProcessDpiAwareness`,
enforces single-instance (second launch exits immediately), and shuts down cleanly.
One MSVC-specific fix required: the linker's auto-generated manifest (resource id 1)
collides with our `app.rc`-embedded one — needs `/MANIFEST:NO` (already in `CMakeLists.txt`).

## Phase 1 — Foreground-window proof of concept (no tray yet)

- [ ] `src/toggle/RectHistory.h/.cpp` — pure toggle state machine (`RecordObservedRect`, `Toggle`, programmatic-move suppression)
- [ ] `tests/RectHistoryTests.cpp` — unit tests for the above
- [ ] `src/windowtracking/RectUtils.h/.cpp` — DWM extended-frame-bounds ↔ window-rect conversion, epsilon-equality
- [ ] `tests/RectUtilsTests.cpp` — unit tests for the above
- [ ] `src/windowtracking/TitleBarInfo.h/.cpp` — `WM_GETTITLEBARINFOEX` query via `SendMessageTimeout`
- [ ] `src/overlay/OverlayWindow.h/.cpp` — layered popup: position, hit-test, click callback, hover
- [ ] `src/overlay/ButtonRenderer.h/.cpp` — GDI+ button glyph rendering
- [ ] `src/hotkey/HotkeyManager.h/.cpp` — global hotkey registration wired to `RequestToggle`
- [ ] Wire foreground-window tracking: `EVENT_SYSTEM_FOREGROUND` + `LOCATIONCHANGE` + `MOVESIZESTART/END`, single tracked window
- [ ] Verify `TITLEBARINFOEX.rgrect` index assumptions with Spy++
- [ ] Manual smoke test: Notepad/Explorer (button works), Windows Terminal/Chrome (button skipped, hotkey still toggles)

## Phase 2 — Full multi-window tracking

- [ ] `src/windowtracking/WinEventHookManager.h/.cpp` — RAII `SetWinEventHook`/`UnhookWinEvent`, full event set
- [ ] `src/windowtracking/WindowRegistry.h/.cpp` — tracked-window map, `EnumWindows` seed, candidate-window filtering
- [ ] `src/windowtracking/TrackedWindow.h` — per-window state struct
- [ ] Settle-timer debounce (~150–200ms) as universal commit path; `MOVESIZEEND` fast path
- [ ] Reconciliation sweep (`IsWindow()` over tracked map every 2–5s)
- [ ] Z-order re-assertion on `FOREGROUND`/`REORDER`/`SHOW`
- [ ] Per-DPI button bitmap cache; `GetDpiForWindow` check on location-settle
- [ ] Minimize/cloak handling (hide overlay, don't record sentinel rects)
- [ ] Manual multi-window test matrix (see README/test matrix in design doc)

## Phase 3 — Tray, autostart, settings, polish

- [ ] `src/tray/TrayIcon.h/.cpp` — `Shell_NotifyIconW`, context menu, `TaskbarCreated` re-registration
- [ ] `src/tray/AutoStart.h/.cpp` — `HKCU...\Run` read/write, `StartupApproved` awareness
- [ ] `src/settings/Settings.h/.cpp` — `%LOCALAPPDATA%` config file, global enable/disable, exclusion list
- [ ] `src/util/ProcessInfo.h/.cpp` — PID → image name for exclusion matching
- [ ] Configurable global hotkey with `RegisterHotKey` conflict detection surfaced in tray UI
- [ ] "Restart as Administrator" tray menu item (`ShellExecute` `"runas"`, exit current instance)
- [ ] Theme-aware redraw on `WM_SETTINGCHANGE`/`ImmersiveColorSet`
- [ ] Full `docs/LIMITATIONS.md` writeup
- [ ] Full manual test matrix pass (all rows in design doc)

## Backlog / future features

- [ ] (nothing yet — add ideas here as they come up, beyond the toggle-button feature)

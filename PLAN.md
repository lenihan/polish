# Polish — Build Plan

Living checkbox tracker for Polish, a Windows 11 utility that keeps native
Snap and native Maximize/Restore working together correctly (see the
2026-08-29 pivot below for what that means and why). Check items off in
the same commit that completes them. Design rationale and full technical
detail for Phase 0 live in the design doc this was generated from; this
file is the at-a-glance progress tracker, and is now also the source of
truth for the architecture pivots described below (the original design
doc is stale on the interaction model as of 2026-08-28, and stale on the
core feature itself as of 2026-08-29).

See [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) for known, permanent
limitations (not bugs to fix).

## Architecture pivot (2026-08-29): custom UI → native restore-position sync

**The whole "custom UI to trigger an action" direction (Shift+click-
maximize with a hover hint, then the Shift+Right-click radial menu that
replaced it) is set aside.** The user reframed the actual problem while
the radial menu was mid-implementation: Windows' native Maximize/Restore
button pair reads from and writes to a per-window "restore rect"
(`WINDOWPLACEMENT.rcNormalPosition`), and Snapping a window (drag-to-edge,
Win+Arrow, Snap Layouts) does not reliably update that value the way an
ordinary move/resize does. Concretely: snap a window to a quarter, click
Maximize, click Restore — you land back at wherever the window was
*before* you ever snapped it, not at the quarter snap. That's the actual
annoyance. The fix isn't a new gesture to remember; it's keeping
`rcNormalPosition` correctly synced to the window's last real position
so the *native*, already-familiar Maximize/Restore buttons just do the
right thing, with zero new UI.

**Implementation**: `SyncRestorePlacement(hwnd, rect)` (in `main.cpp`) —
`GetWindowPlacement`, overwrite `rcNormalPosition` with the just-settled
rect, `SetWindowPlacement` back with everything else (crucially,
`showCmd`) unchanged. Wired into the single existing choke point all
settle commits already went through, `CommitRectForTrackedWindow` — one
new call, reusing 100% of the settle-detection machinery already built
and hardened this session (drag-gating via `MOVESIZESTART`/`g_inMoveSizeLoop`,
the debounce timer, eager-commit-on-distinct-change). No new hooks, no
new windows, no new gestures.

**A real, adjacent bug found and fixed while wiring this in**: neither
`CheckSettledRectAndRecord` nor the `LOCATIONCHANGE` handler had ever
guarded against the window being *maximized* (only `IsIconic`/minimized
was checked) — maximizing fires `LOCATIONCHANGE` too, and without a
guard, the full-screen rect would get treated as a legitimate settled
position and (with this new fix) written into `rcNormalPosition` as the
*next* restore target, which is the exact opposite of correct. Fixed
with a new `IsWindowInNormalState(hwnd)` helper (`!IsIconic && !IsZoomed`)
used everywhere a rect gets treated as "the window settled here" — both
in `CheckSettledRectAndRecord` and in the `LOCATIONCHANGE` handler's
pending-rect tracking, plus a defensive re-check of `showCmd` inside
`SyncRestorePlacement` itself.

**Verified end-to-end** with a *genuine* Win+Left snap (not a simulated
`SetWindowPos` call, which turned out to be an invalid test — see the
debugging note below) on File Explorer: settle correctly recorded,
`rcNormalPosition` correctly matched the snapped rect via direct
`GetWindowPlacement` query, and a native `SW_MAXIMIZE` → `SW_RESTORE`
round-trip landed exactly back at the snapped rect. Not yet confirmed by
the user in their own original real-world scenario (the whole reason for
this pivot) — that's the test that actually matters.

**Debugging note, kept because it's a real methodology lesson**: an
initial round of automated verification silently proved nothing, because
`g_trackedWindow` wasn't actually engaged (no `EVENT_SYSTEM_FOREGROUND`
had fired for the test window in a while, likely due to intervening
focus changes between separate tool invocations in this environment) —
yet `rcNormalPosition` still matched perfectly, because a plain
`SetWindowPos` call (used to *simulate* a snap) turns out to already be
kept in sync by Windows natively, with no help from Polish. The tell was
the complete absence of "settled rect recorded" log lines despite the
rect visibly changing — a reminder that "the result looks right" isn't
sufficient confirmation the code under test actually ran; check the log
for evidence the code path was hit, not just the final state. Root
cause, once investigated: the `"foreground changed: hwnd=... candidate=..."`
diagnostic log line that made this exact kind of gap visible earlier in
the session had been silently dropped during a prior refactor and was
never re-added. Restored (permanently, not just as a throwaway
diagnostic — it's genuinely useful state-transition logging) as part of
this fix.

- [x] `main.cpp`: `SyncRestorePlacement`, `IsWindowInNormalState`, wired
      into `CommitRectForTrackedWindow` and the `LOCATIONCHANGE` handler
- [x] Restored the dropped `OnForegroundChanged` diagnostic log line
- [x] Build + unit tests green (13/13 — no logic in `RectHistory`/
      `RectUtils` changed)
- [x] Verified end-to-end via direct `WINDOWPLACEMENT` queries + a real
      Win+Left snap + native maximize/restore round-trip (see above)
- [ ] **Not yet confirmed by the user in their own original scenario.**

**Follow-up bug found by the user: "works well for new windows....existing
windows dont always work."** Root cause: the sync above is purely
*reactive* — it only fires when `CommitRectForTrackedWindow` observes a
**new** settle. A window that was already sitting at a snapped position
*before* Polish ever started tracking it (an "existing" window the user
brings to foreground and, without moving it any further, just clicks the
native Maximize then Restore buttons on) never produces a settle event at
all, since its rect never changes after Polish starts watching it — so its
stale, pre-Polish `rcNormalPosition` was never corrected. "New" windows
happened to work because the user's natural test workflow was to actively
re-snap them right after launch, which *does* go through the reactive
path. Fixed by making `EnsureHistoryEntryExists` (which runs once per
window, the first time it's seen — at Polish startup for whatever's
already foreground, and on every subsequent `EVENT_SYSTEM_FOREGROUND`)
also proactively call `SyncRestorePlacement` with the window's current
rect right there, gated by the same `IsWindowInNormalState` check, instead
of waiting for a future change that may never come.

Verified with a deliberately "existing window" repro: launched and
Win+Left-snapped Notepad *before* starting Polish at all, then started
Polish (which picks up the already-foreground Notepad via the
`OnForegroundChanged(GetForegroundWindow())` call in `wWinMain`) —
`rcNormalPosition` was already correctly synced to the snapped rect
immediately after Polish's startup line in the log, with no settle event
needed, and a native `SW_MAXIMIZE` → `SW_RESTORE` round-trip landed
exactly back at the snapped rect.

- [x] `EnsureHistoryEntryExists` proactively syncs on first-seen, not just
      on the next reactive settle
- [x] Build + unit tests green (13/13)
- [x] Verified end-to-end with a pre-Polish-snapped window repro (above)

**Radial menu / Shift+Right-click trigger: deleted.** Initially shelved
(kept on disk, out of the build) while confirming the restore-sync fix
was the right direction; once confirmed, `src/hook/RadialMenu.*` and
`src/hook/TitleBarMenuHook.*` were deleted outright rather than kept
around unused. Never committed, so there's no git history to recover
them from — reviving this trigger means reimplementing it from the
design notes in the "Architecture pivot (2026-08-28)" section below
(gesture, hit-testing, rendering pipeline, and the real bugs found are
all documented there even though the code itself is gone).

**Toggle (`RectHistory`, `HotkeyManager`, Win+Alt+T) also deleted.** With
the radial menu gone, the hotkey was Toggle's only remaining trigger, and
with the user deciding they don't need the hotkey either, Toggle had no
trigger left at all. Rather than leave it as unreachable dead code (same
reasoning as deleting the radial menu above), removed entirely:
`src/hotkey/HotkeyManager.*`, `src/toggle/RectHistory.*`, and
`tests/RectHistoryTests.cpp`, plus all the toggle-tracking scaffolding in
`main.cpp` (`g_rectHistories`, `OnToggleRequested`, `OnHotkeyPressed`,
`g_lastToggledWindow`, the `kProgrammaticMoveTimeoutTimerId` safety net,
`WM_HOTKEY` handling). Polish is now purely the restore-position-sync
fix: track the foreground window's settled rects, keep
`rcNormalPosition` synced, nothing else.

This also simplified the settle-commit path: `EnsureHistoryEntryExists`
(which needed a per-window map to know whether a window had been seen
before) is replaced by `SyncRestorePlacementNow(hwnd)`, called
unconditionally from `OnForegroundChanged` on every focus change, not
just the first one for a given window. That's safe because syncing
`rcNormalPosition` to a window's own current rect while it's already in
that exact state is idempotent — no per-window "have we seen this before"
state is needed at all anymore.

- [x] Deleted `src/hotkey/`, `src/toggle/`, `tests/RectHistoryTests.cpp`
- [x] `CMakeLists.txt` / `tests/CMakeLists.txt` updated accordingly
- [x] `main.cpp` simplified: no more `g_rectHistories`/toggle state;
      `SyncRestorePlacementNow` replaces `EnsureHistoryEntryExists`
- [x] Build + unit tests green (3/3 — `RectUtils` only, `RectHistory`'s
      10 tests removed along with the code they tested)
- [x] `README.md` / `docs/LIMITATIONS.md` updated to drop Toggle/hotkey
      mentions

## Architecture pivot (2026-08-28): overlay button → Shift+click maximize

The original design (a custom always-visible toggle button drawn next to
the minimize button, via a per-window layered overlay window) is
**abandoned**. Real hands-on testing surfaced a reproducible bug: the
button would appear on a freshly-focused window, then disappear
permanently after being clicked and never return, because clicking the
(`WS_EX_NOACTIVATE`) overlay appears to trigger a spurious foreground-change
event that tore down Phase 1's fragile foreground-only tracking. On top
of that bug, the overlay was already the single buggiest, most
Win32-subtlety-laden part of the app (see the two real bugs logged under
old Phase 1 below).

**New interaction model:**

- **Shift+click a window's own (real) maximize/restore button** triggers
  the toggle instead of the normal maximize/restore action. Implemented
  via a low-level mouse hook (`WH_MOUSE_LL`), which watches clicks
  system-wide, checks whether Shift is held and the click landed inside
  the target window's maximize-button rect (already had the code to find
  that, via `WM_GETTITLEBARINFOEX`), and if so swallows the click and
  runs the toggle instead of letting it reach the window.
- The **global hotkey** (unchanged, already reliable) remains as a second
  way to trigger the toggle, and is the only option for windows with no
  real maximize button to click (custom-drawn titlebars).
- A **system tray icon** now exists so there's visible confirmation the
  app is running, and a way to quit it without Task Manager (a real gap
  in Phase 1 — there was no UI of any kind).
- A **hover indicator**: while Shift is held and hovering a maximize
  button, a real Win32 tooltip control (`HoverIndicatorWindow`, `TTF_TRACK`,
  manually positioned/activated) appears just below the button, saying
  "Toggle to previous size" or "No previous size to toggle to yet".
  Answers "will this do anything" before clicking, which is otherwise
  undiscoverable. Two earlier attempts at this didn't pan out -- see
  below -- so it was replaced with this approach. Not load-bearing for
  the toggle mechanism itself.

This removes `src/overlay/` (`OverlayWindow`, `ButtonRenderer`) entirely
— no per-window overlay windows, no z-order bookkeeping, no visible-button
lifecycle to get wrong — while keeping everything that was actually
reliable through all of Phase 1's testing: `RectHistory`, `RectUtils`,
`TitleBarInfo` (generalized to query any caption button, not just
minimize), and `HotkeyManager`.

**As part of this pivot, per-window toggle history now persists across
focus changes** (a `std::unordered_map<HWND, RectHistory>` keyed by HWND,
pruned on `EVENT_OBJECT_DESTROY`), fixing a known Phase 1 limitation where
switching away from a window and back lost its history. Windows are still
only *observed* (rect changes recorded) while they're the foreground
window — true always-on multi-window tracking is still Phase 2 scope.

## Architecture pivot (2026-08-29): Shift+click-maximize → Shift+Right-click title bar radial menu

The Shift+click-maximize-button mechanism (with its `HoverIndicatorWindow`
tooltip hint) is **superseded**. It wasn't broken exactly — the underlying
toggle worked correctly once clicked — but the hint could never be made
reliable: Windows 11 shows its own Snap Layouts flyout on hover over any
real maximize button, independent of any modifier key, on its own timer.
That's not a bug to fix, it's two features racing for the same hover zone
by construction. Three consecutive attempts at a visible hint (cursor
swap, drawn highlight, tracked tooltip) all failed for different reasons
before this was understood clearly enough — see the "Real bugs" writeups
below for the full history, kept because the underlying Win32 facts
remain true and are easy to relearn the hard way.

The user also wanted a richer, more discoverable, primarily-mouse-driven
way to invoke window-placement actions generally (not just toggle) —
snap directions, maximize, minimize — which prompted a design discussion
resolved as:

- **Trigger: Shift+Right-click anywhere on the title bar**, not the
  maximize button specifically. This is the structural fix for the
  flyout-collision problem — the title bar generally isn't a zone
  Windows' Snap flyout watches, only the specific maximize-button
  hit-test region is, so this trigger cannot race against it. Right-click
  carries "show me options" as an existing mental model; Shift avoids
  overriding the system-menu right-click most windows already have.
- **Interaction: click-to-open, click-to-select**, not press-hold-drag-
  release. A drag-based confirm (the more "classic" radial-menu gesture)
  was considered and rejected — hard to be confident it wouldn't
  interfere with apps that support their own drag-and-drop or
  right-click-drag gestures. Shift+Right-click opens the menu (pinned
  open); a plain left-click on a wedge selects it; clicking outside or
  Escape dismisses without acting.
- **Visual style: a radial (pie) menu** (vs. a full-screen overlay with
  large buttons) — smaller, appears at the cursor, better fit for a
  frequent action.
- **Action set (v1)**: Toggle, Maximize, Minimize, Snap Left half, Snap
  Right half — 5 wedges. Quarter-snaps considered but deferred (more
  wedges = harder to hit accurately; add only if 5 feels like it needs
  more).

`src/hook/MaximizeClickHook.*` and `src/hook/HoverIndicatorWindow.*` are
deleted (not just superseded in text) — replaced by
`src/hook/TitleBarMenuHook.*` (trigger detection, target-window
resolution, dispatch) and `src/hook/RadialMenu.*` (the popup: rendering,
wedge hit-testing, highlight state). `src/windowtracking/TitleBarInfo.*`
(the `WM_GETTITLEBARINFOEX` query) is also deleted — nothing needs to
locate the *real* maximize button anymore, since the new trigger uses a
DPI-scaled title-bar-width band instead (works identically for real- and
custom-titlebar apps, no per-app detection needed at all). The global
hotkey (`Win+Alt+T`, direct toggle, no menu) is untouched.

`RadialMenu`'s rendering reuses the same DIB + `Gdiplus::Bitmap`
(external-buffer, `PixelFormat32bppARGB`) + manual premultiply +
`UpdateLayeredWindow` pipeline proven out by the earlier overlay-button
work (`Graphics(HDC)` does not reliably preserve alpha — see the "Real
bugs" entry below). Unlike that earlier overlay, `WS_EX_TOPMOST` is
correct and safe here without qualification: the menu is a deliberately-
triggered, short-lived, dismissible popup with no owner-window z-order
relationship to maintain, so none of the "topmost fights with owner
z-order" concerns that bit the old always-on overlay apply.

- [x] `src/hook/RadialMenu.h/.cpp` — the popup: 5-wedge pie rendering
      (`Gdiplus::Graphics::FillPie`/`DrawPie`, 0°=top/12-o'clock
      increasing clockwise as the logical convention, converted to GDI+'s
      0°=east convention via a -90° offset), angle+radius-based
      `HitTest` (center dead zone + outer bound both return
      `std::nullopt`, meaning "dismiss without acting" to the caller),
      `SetHighlightedAction` (redraws only on actual change), DPI-scaled
      sizing computed at `Show()` time (position first at a placeholder
      size, query `GetDpiForWindow`, then resize — avoids a separate
      `MonitorFromPoint`/`GetDpiForMonitor`/shcore.dll round-trip)
- [x] `src/hook/TitleBarMenuHook.h/.cpp` — `WH_MOUSE_LL` + `WH_KEYBOARD_LL`
      (Escape only, swallowed only while the menu is actually open):
      Shift+Right-click detection via a DPI-scaled full-width title-bar
      band (`ResolveTitleBarRect`, adapted from the old
      `EstimateControlsRegionRect` but spanning the whole title bar
      instead of a top-right corner), open/hover-highlight/select/dismiss
      state machine, `GetAsyncKeyState`-based Shift detection (not
      `GetKeyState`, same reasoning as before — this hook doesn't process
      keyboard messages itself)
- [x] `src/main.cpp` — `OnRadialMenuAction` dispatcher (Toggle reuses the
      existing `OnToggleRequested`; Maximize/Minimize via `ShowWindow`;
      `SnapLeft`/`SnapRight` via a new `SnapToHalf` using
      `MonitorFromWindow`/`GetMonitorInfoW` for the work-area rect of
      whichever monitor the window is currently on); replaced
      `g_maximizeClickHook` with `g_titleBarMenuHook`
- [x] `CMakeLists.txt` — re-added `gdi32`/`gdiplus` to `polish_core` (had
      been removed when the drawn-highlight overlay was replaced by the
      tooltip; needed again for the radial menu's rendering)
- [x] Build + unit tests green after the pivot (13/13 — `RectHistory`/
      `RectUtils` untouched by this change)
- [ ] **Not yet human-tested.** Build-verified only (clean compile, hook
      installs without error at startup per the log). Manual test matrix
      is in the design plan this pivot came from
      (`C:\Users\david\.claude\plans\i-want-to-make-compressed-dusk.md`
      at the time of writing, if still relevant when read later) — covers
      trigger-on-real-vs-custom-titlebar apps, per-wedge hover/click,
      dismiss-via-outside-click/Escape, and confirming plain (non-Shift)
      right-click still reaches the normal system menu unregressed.

## Phase 0 — Scaffolding

- [x] `README.md`
- [x] `PLAN.md` (this file)
- [x] `setup.ps1` bootstrap script (detect/install VS2022 + C++ workload, CMake via winget)
- [x] `CMakeLists.txt` (C++23, `WIN32_EXECUTABLE`, `UNICODE`, `_WIN32_WINNT=0x0A00`)
- [x] `app.manifest` (Per-Monitor-V2 DPI awareness, `asInvoker`, Win10/11 `supportedOS` GUIDs) — note: the `dpiAwareness` element must use the `http://schemas.microsoft.com/SMI/2016/WindowsSettings` namespace, not 2017 (that's for `longPathAware`); using 2017 fails activation silently-ish (app still launches, but falls back to DPI-unaware — verify with `GetProcessDpiAwareness`, not just "did it start")
- [x] `resources/app.rc` (`RT_MANIFEST` embed; custom icon still deferred — tray currently uses a stock system icon)
- [x] `src/main.cpp` skeleton: `WinMain`, single-instance mutex, DPI-awareness verification, message loop
- [x] `docs/LIMITATIONS.md` stub
- [x] `CMakePresets.json` — short, memorable CLI (`cmake --preset default`,
      `cmake --build --preset default` / `debug`, `ctest --preset default`)

Verified end-to-end: `cmake -B build -S .` + `cmake --build build --config Release` succeeds;
built exe launches, is confirmed `PROCESS_PER_MONITOR_DPI_AWARE` via `GetProcessDpiAwareness`,
enforces single-instance (second launch exits immediately), and shuts down cleanly.
One MSVC-specific fix required: the linker's auto-generated manifest (resource id 1)
collides with our `app.rc`-embedded one — needs `/MANIFEST:NO` (already in `CMakeLists.txt`).

## Phase 1 — Toggle mechanism + visible presence

- [x] `src/toggle/RectHistory.h/.cpp` — pure toggle state machine (`RecordObservedRect`, `Toggle`, programmatic-move suppression). `Toggle()` updates state *eagerly* (before Win32 confirmation) so rapid double-clicks toggle correctly without waiting on WinEvent latency — see doc comments.
- [x] `tests/RectHistoryTests.cpp` — 10 unit tests, all passing; caught a real bug in how an unrelated rect arriving mid-toggle interacts with the eager update (fixed in test expectations + documented, not a logic bug)
- [x] `src/windowtracking/RectUtils.h/.cpp` — DWM extended-frame-bounds ↔ window-rect conversion, epsilon-equality
- [x] `tests/RectUtilsTests.cpp` — 7 unit tests, all passing
- [x] `polish_core` static library + `tests/CMakeLists.txt` (doctest via `FetchContent`) so logic is shared between the app and `polish_tests.exe`
- [x] `src/windowtracking/TitleBarInfo.h/.cpp` — `WM_GETTITLEBARINFOEX` query via `SendMessageTimeout`, generalized to a `CaptionButton` enum (Minimize/Maximize/Close) rather than minimize-only
- [x] `src/hotkey/HotkeyManager.h/.cpp` — global hotkey registration wired to the toggle
- [x] `src/util/Logging.h` — minimal file+debugger logger (pulled forward from Phase 3's planned util layer; needed immediately for debugging a headless/background app with no console)
- [x] ~~`src/overlay/OverlayWindow.h/.cpp`, `src/overlay/ButtonRenderer.h/.cpp`~~ — **removed**, see architecture pivot above
- [x] `src/hook/MaximizeClickHook.h/.cpp` — `WH_MOUSE_LL` low-level mouse hook: Shift+click (left or right button) on a real maximize button swallows the click and triggers toggle; also drives the hover indicator
- [x] `src/hook/HoverIndicatorWindow.h/.cpp` — the transient highlight-rect overlay described above; reuses the proven DIB+`Gdiplus::Bitmap`+premultiply+`UpdateLayeredWindow` rendering technique from the (now-removed) original overlay button, since that part always worked correctly
- [x] `src/tray/TrayIcon.h/.cpp` — `Shell_NotifyIconW`, right-click "Exit" menu, `TaskbarCreated` re-registration (pulled forward from Phase 3 — needed immediately so the app is visible/controllable at all)
- [x] Per-window `std::unordered_map<HWND, RectHistory>` (replaces the single global from the original Phase 1), pruned on `EVENT_OBJECT_DESTROY`
- [x] Wire foreground-window tracking: `EVENT_SYSTEM_FOREGROUND` + `LOCATIONCHANGE` + `MOVESIZEEND` + `OBJECT_DESTROY`
- [x] Verify `TITLEBARINFOEX.rgrect` index assumptions — validated empirically (not via Spy++, which isn't practical in this environment): logged button screen rects matched the real button position confirmed via screenshot pixel sampling, for both File Explorer and modern Notepad
- [x] Build + unit tests green after the pivot (all 17 tests, unaffected since `RectHistory`/`RectUtils` didn't change)
- [x] **First human smoke test found a real bug**: toggling between a Windows-Snap quarter and half position restored the correct *size* but the wrong *position*. Root-caused and fixed — see below. Fix is build-verified (13/13 tests pass; `RectUtils` lost 4 tests along with the removed code) but not yet re-confirmed by the human tester against the fix.
- [x] Human testing found and fixed two more real issues: (1) the hook only handled left-click, but the user naturally tried Shift+**right**-click on the maximize button, which has no default Windows behavior to begin with — now both buttons are supported; (2) added a two-state cursor hint (hand vs. "no") driven by `RectHistory::HasToggleTarget()`, so it's visible *before* clicking whether a window has anything to toggle to yet, directly addressing confusion where a window's first-ever observed position has no history to toggle back to.
- [x] **The hover indicator itself went through three attempts before working**, each invisible for a different reason — see the dedicated writeup below. Human testing then confirmed the working (3rd) version end-to-end: tooltip correctly appears for File Explorer/Notepad (real maximize button found), and correctly does *not* appear for VS Code/the Claude desktop app (both custom-titlebar, `maximizeRect=none` logged every time) — exactly the documented limitation, not a bug.
- [ ] **Manual smoke test of the tray icon's Exit item** — still unconfirmed.
- [x] **Custom-titlebar fallback**: for windows with no real, discoverable maximize button (VS Code, Claude desktop, Windows Terminal, WinUI3 apps, etc.), Shift+hover/click now falls back to an estimated hit zone in the top-right corner (`EstimateControlsRegionRect` — DPI-scaled ~200×32px) instead of doing nothing, so these apps get the same hover indicator + click-to-toggle as apps with a real button, without needing to precisely locate their custom-drawn controls (safe to be imprecise since the gesture is Shift-gated and swallowed before the app ever sees it). Logged as `maximizeRect=... (estimated)` to distinguish from a real button match. **Human-confirmed working** for VS Code and the Claude desktop app.
- [ ] **Indicator is now clickable, not just informational (unconfirmed)**: the user asked to add a button to *Windows' own* Snap Layouts flyout, which isn't feasible (undocumented shell-owned popup, no extension API, would almost certainly be blocked by process/session isolation even if attempted). Instead, `HoverIndicatorWindow::GetScreenRect()` exposes the tooltip's own on-screen bounds, and `MaximizeClickHook` now treats a click there the same as a click on the button/zone it's anchored to, and no longer hides the indicator when the cursor moves onto it (previously it would vanish the instant you tried to reach it). Not yet tried by a human.
- [x] **Two follow-up problems reported, both addressed**:
  - Fixed, human-confirmed: the indicator only appeared if Shift was already held *before* the cursor entered the button. Added a `WH_KEYBOARD_LL` hook (Shift transitions only, never swallowed) that re-checks the indicator at the current cursor position on every Shift transition.
  - Investigated (user hadn't actually confirmed click-through-when-covered yet when this was addressed): asked the user to isolate the "covered by Snap flyout" question from a genuinely different bug by testing snap-quarter → snap-half → toggle on a real-button window. Their test surfaced **the real bug**, documented next.
- [x] **Real bug: settle-tracking silently dropped positions during a paused drag.** User's test (Notepad: snap quarter, snap half, toggle) went to a size from *before* either snap on the first click, not to quarter — meaning quarter was never recorded as a history entry at all. Log evidence: 6 separate "settled rect recorded" entries for one tracked window in quick succession, two of them sharing the exact same size (2167×1367) but different (large, partly off-screen) positions — the signature of an in-progress drag, not discrete snap actions. Root cause: the 180ms settle-debounce timer was never gated by whether a drag was actually in progress. We listened for `EVENT_SYSTEM_MOVESIZEEND` but had never subscribed to `EVENT_SYSTEM_MOVESIZESTART`, so we had no way to know "a drag is currently happening." Pausing briefly mid-drag (very normal) let the debounce timer fire on an incidental in-between position, recording it as if deliberate and — with only two history slots — bumping the real "quarter" state out before "half" was ever recorded. **Fixed**: now subscribe to `MOVESIZESTART` too, track `g_inMoveSizeLoop`, and suppress the settle timer entirely while true (`MOVESIZEEND` remains the sole authoritative commit point during a live drag). Also added eager-commit-on-distinct-change (`g_pendingSettleRect`, compared via the existing `RectsApproximatelyEqual`) for the non-drag case, so two quick successive discrete changes (e.g. keyboard snaps) each still get recorded even if the second arrives before the first's debounce fires. Build-verified (13/13 tests unaffected — `RectHistory`/`RectUtils` didn't change, only how `main.cpp` decides *when* to call `RecordObservedRect`); not yet re-confirmed by the user against the exact repro sequence.
- [x] The "toggle only remembers the last rect" concern from earlier is very likely explained by the "first-observed-position has no history" gap (now directly visible via the hover indicator's "No previous size to toggle to yet" text) rather than a distinct bug — log analysis showed correct alternation between two rects across 5 toggles for one window. Not separately reproduced/confirmed with the user beyond that, but the hover indicator should make this self-diagnosable going forward.

**Real bugs found and fixed** (kept here for history/reference — the underlying Win32 facts are still true and could bite again in future UI work):

- `SetWindowPos`'s `hWndInsertAfter` places a window *behind* the given handle, not in front of it (the handle "precedes" ours in the front-to-back Z order). (Found in the now-removed overlay code.)
- `Gdiplus::Graphics(HDC)` falls back to GDI-compatibility rendering and does not reliably write alpha into a backing DIB. (Found in the now-removed overlay code.)
- The `dpiAwareness` manifest namespace bug from Phase 0 (`2016` vs `2017`).
- **Toggling restored the correct size but the wrong position** for windows toggled between two Windows-Snap states (e.g. quarter ↔ half). Cause: `RectHistory` recorded DWM extended-frame-bounds ("visual bounds") rects and converted back to `GetWindowRect`-style coordinates via a computed inset at restore time. That conversion assumed the invisible resize-border inset is a fixed per-window constant — it isn't; Windows appears to compute it differently for edges flush against a snap boundary than for a free-floating edge, so the inset queried at *toggle time* (reflecting the *current* snap state) didn't match the inset in effect when the *target* rect was originally recorded (a different snap state), producing a few pixels of consistent position offset. **Fixed by removing the frame-bounds conversion entirely** — `RectHistory` now records and restores raw `GetWindowRect()` values directly. This is correct by construction: replaying the exact rect a window already reported for itself needs no correction, regardless of how DWM computes its invisible border. Removed `FrameInsets`, `ComputeFrameInsets`, `FrameRectToWindowRect`, `WindowRectToFrameRect`, and `QueryFrameInsets` from `RectUtils` (and their tests) as now-unused; kept `RectsApproximatelyEqual`, which `RectHistory` still uses. See the comment at the top of `RectUtils.h` for the full explanation, written so this doesn't get reintroduced later.
- **The hook only handled left-click**; the user naturally tried Shift+right-click on the maximize button instead. Fixed by handling both `WM_LBUTTONDOWN`/`UP` and `WM_RBUTTONDOWN`/`UP` symmetrically (tracking which button's down was swallowed, so the matching button's up is what gets swallowed).
- **`SetCursor`-based hover hinting didn't work reliably.** The idea (swap to a hand/no cursor while Shift-hovering a maximize button) was sound, but many target windows reassert their own cursor via `WM_SETCURSOR` on their own subsequent mousemove processing, which can win the race against a hook-driven `SetCursor` call happening earlier in the pipeline — the net effect was inconsistent or invisible.
- **A drawn highlight rect on top of the maximize button was invisible too, for a different reason**: Windows 11 shows its own Snap Layouts flyout on hover over *any* maximize button, independent of Shift, on its own hover-delay timer — an opaque shell-rendered popup that covers anything drawn in that same spot once it appears. **Fixed by switching to a real Win32 tooltip control** (`HoverIndicatorWindow`, `TTF_TRACK`), positioned just below the button rather than on top of it, and activated immediately via `TTM_TRACKACTIVATE` (bypassing the built-in hover delay normal tooltips have) so it has a chance to be seen before the native flyout, if it appears at all, takes over.
- **The tooltip control itself was then invisible too — a third, unrelated cause.** `TTM_ADDTOOL` was silently failing (returned 0) because `app.manifest` never declared a dependency on Common Controls v6 (`Microsoft.Windows.Common-Controls`, version `6.0.0.0`). Without that manifest dependency, comctl32 loads against an older/classic ABI whose `TOOLINFO` struct size doesn't match the `sizeof(TOOLINFOW)` our SDK headers use for `cbSize`, so the OS rejects the call. This is a well-known, classic Win32 gotcha with common controls generally (not specific to tooltips), worth remembering for *any* future comctl32 usage in this app. Diagnosed by adding logging around every step of the hover pipeline (window/rect detection, `ShowAt` calls, `TTM_ADDTOOL`/`TTM_TRACKACTIVATE` return values) after three consecutive "nothing visible" reports made it clear guessing at rendering techniques wasn't working — the logging immediately showed `TTM_ADDTOOL result=0`. Fixed by adding the `<dependency>` block to `app.manifest`; confirmed with `TTM_ADDTOOL result=1` after the fix, and human testing confirmed the tooltip visible for File Explorer/Notepad and correctly absent for VS Code/Claude (custom titlebars, `maximizeRect=none`).

**Testing-environment caveats worth knowing for future sessions:**

- This is a **live, shared desktop session** (confirmed via screenshot — VS Code, the user's own terminal, and a live "Remote Control" session are all present), not an isolated sandbox. Earlier automated-click tests in this session showed a reproducible "phantom window steals foreground" artifact when simulating clicks; given the environment turned out to be shared, this was very likely synthetic input landing on real desktop UI (e.g. VS Code) rather than a sandbox quirk. **Prefer asking the human to test interactively over further synthetic-input automation in this environment.**
- PowerShell tool invocations are DPI-unaware by default and each is a fresh process — `SetThreadDpiAwarenessContext(-4)` must be set at the top of *every* script that touches window rects/positions, or coordinates get silently virtualized (typically ~2x off on this machine's 200%-scaled display).
- This machine already has something claiming a broad range of `Win+Alt+<letter>` hotkeys (both `Win+Alt+T` and `Win+Alt+Y` were rejected with `ERROR_HOTKEY_ALREADY_REGISTERED`) — real-world validation that Phase 3's conflict-detection UI is necessary, not speculative. The shipped default is `Win+Alt+T`; it may need to be changed on this machine, or used via the tray/Shift-click path instead.
- Windows 11 hides newly-added tray icons in the "^" overflow flyout by default — not finding the icon in the main tray strip is expected, not a bug.

## Phase 2 — Full multi-window tracking

- [ ] `src/windowtracking/WinEventHookManager.h/.cpp` — RAII `SetWinEventHook`/`UnhookWinEvent`, full event set
- [ ] `src/windowtracking/WindowRegistry.h/.cpp` — tracked-window set, `EnumWindows` seed, candidate-window filtering (there's no per-window map to reuse anymore now that `RectHistory` is deleted — restore-position sync needs no stored state per window, just `IsWindowInNormalState` + `GetWindowRect` at settle time — so Phase 2 is purely about triggering that same sync for every tracked window, not just the foreground one)
- [ ] `src/windowtracking/TrackedWindow.h` — per-window state struct
- [ ] Settle-timer debounce (~150–200ms) as universal commit path; `MOVESIZEEND` fast path — already implemented for the single foreground window; generalize to all tracked windows
- [ ] Reconciliation sweep (`IsWindow()` over tracked map every 2–5s)
- [ ] `GetDpiForWindow` check on location-settle (DPI-dependent behavior, if any is added later)
- [ ] Minimize/cloak handling refinements (currently gated on `IsIconic` only)
- [ ] Manual multi-window test matrix (see original design doc)

## Phase 3 — Settings, polish, autostart

- [x] `src/tray/TrayIcon.h/.cpp` — done in Phase 1 (pulled forward)
- [ ] Custom tray icon (currently a stock system icon)
- [ ] `src/tray/AutoStart.h/.cpp` — `HKCU...\Run` read/write, `StartupApproved` awareness
- [ ] `src/settings/Settings.h/.cpp` — `%LOCALAPPDATA%` config file, global enable/disable, exclusion list
- [ ] `src/util/ProcessInfo.h/.cpp` — PID → image name for exclusion matching
- [x] ~~Configurable global hotkey with `RegisterHotKey` conflict detection~~ — moot: the global hotkey (and Toggle, its only remaining purpose) was deleted entirely on 2026-08-29, see the architecture-pivot note above
- [ ] "Restart as Administrator" tray menu item (`ShellExecute` `"runas"`, exit current instance)
- [ ] Full `docs/LIMITATIONS.md` writeup
- [ ] Full manual test matrix pass (all rows in design doc)

## Backlog / future features

- [ ] (nothing yet — add ideas here as they come up, beyond the toggle-button feature)

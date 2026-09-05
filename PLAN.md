# Polish — Build Plan

Living tracker for Polish, a Windows 11 utility that keeps native Snap and
native Maximize/Restore working together correctly: Windows doesn't
update a window's restore position (`WINDOWPLACEMENT.rcNormalPosition`)
when you Snap it, so Maximize-then-Restore after a snap loses it. Polish
fixes that in the background — no new gesture, hotkey, or UI.

See [`README.md`](README.md) for what it does day-to-day and
[`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) for known, permanent
limitations (not bugs to fix). A condensed history of earlier, abandoned
approaches is at the bottom of this file, kept for the Win32 lessons
learned along the way.

## Shipped

- [x] Foreground-window tracking via WinEvent hooks
      (`EVENT_SYSTEM_FOREGROUND`/`MOVESIZESTART`/`MOVESIZEEND`/
      `LOCATIONCHANGE`/`OBJECT_DESTROY`), with drag-gating and a 180ms
      settle debounce so a live drag or a paused mid-drag doesn't produce
      spurious settle events.
- [x] `SyncRestorePlacement`/`SyncRestorePlacementNow` (`src/main.cpp`):
      keeps `rcNormalPosition` synced to the window's last settled rect,
      both reactively (on every settle) and proactively (the moment a
      window is first tracked, so a window that was already snapped
      before Polish started still gets corrected without needing to be
      moved again).
- [x] Tray icon (`src/tray/TrayIcon.*`, `resources/polish.ico`): a
      generated filled 4-point sparkle glyph, replacing the old stock
      `IDI_APPLICATION` icon. User-confirmed it looks right (2026-08-30).
- [x] Single-instance guard, Per-Monitor-V2 DPI awareness, CMake presets,
      unit tests for `RectUtils` (`RectsApproximatelyEqual`).
- [x] **User-confirmed working in real day-to-day use (2026-08-29)** —
      the actual test that mattered, beyond build-verification and
      synthetic repros.

## Next

### 1. Alt+Tab that skips minimized windows (in progress)

**Not achievable by filtering Windows' own Alt+Tab** — the Win11
immersive switcher has no documented API to control its contents. The
one per-window lever, `WS_EX_TOOLWINDOW`, pulls a window out of Alt+Tab
*and* the taskbar together, which isn't what's wanted for a window
that's just minimized and should still be reachable from the taskbar.

So this means **replacing** Alt+Tab, the same pattern real third-party
switcher tools use, and architecturally the same *shape* as the shelved
radial-menu work (a `WH_KEYBOARD_LL` hook plus a custom rendering
surface) — though the rendering itself turns out to be a materially
bigger lift once live thumbnails are in scope (see below).

**Design decisions, resolved 2026-08-30:**

- **Trigger**: `WH_KEYBOARD_LL`, watch for Tab while Alt is down, swallow
  it so Windows' own switcher never appears. Repeated Tab presses while
  Alt stays held cycle the highlight (mirroring native behavior);
  Shift+Tab cycles backward; releasing Alt commits the highlighted window
  via `SetForegroundWindow`; Escape cancels. If the filtered candidate
  list ends up empty (nothing non-minimized to switch to), let the
  keypress fall through to native Alt+Tab instead of swallowing it into a
  dead keystroke.
- **Candidate list**: the same filter `IsCandidateWindow` already uses in
  `main.cpp`, plus excluding anything `IsIconic`.
- **Ordering: true most-recently-used**, matching native Alt+Tab (first
  Tab press jumps to your previous window). Built by extending the
  existing `EVENT_SYSTEM_FOREGROUND` handling to maintain an ordered MRU
  list (move-to-front on every focus change, pruned on
  `EVENT_OBJECT_DESTROY`) — this is a natural extension of tracking
  that's already in place, not new hook surface.
- **Monitor placement**: the monitor containing the current foreground
  window (`MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST)`),
  not the mouse cursor or always-primary — predictable for a
  keyboard-driven gesture regardless of where the mouse happens to be.
- **Rendering: live thumbnails**, matching native Alt+Tab's visual style,
  via `DwmRegisterThumbnail`/`DwmUpdateThumbnailProperties`/
  `DwmUnregisterThumbnail`. This is **not** a reuse of the existing DIB +
  `Gdiplus::Bitmap` + premultiply + `UpdateLayeredWindow` pipeline from
  the radial menu — that pipeline still applies to the switcher's own
  chrome (background, selection highlight, title/icon labels), but each
  live window preview needs its own DWM thumbnail registration, sized
  into a sub-rect of the switcher window, with all of them unregistered
  when the switcher closes. This is genuinely the biggest single feature
  built in this app so far — a new API surface (DWM thumbnails) on top of
  a new hook and a new state machine, not just new wiring of existing
  pieces.
- **Restore-position sync interaction**: resolves itself —
  `SetForegroundWindow` on the selected window fires
  `EVENT_SYSTEM_FOREGROUND` exactly like any other focus change, already
  handled by `OnForegroundChanged`/`SyncRestorePlacementNow`, no special
  casing needed. The switcher popup itself should be `WS_EX_TOOLWINDOW`
  (filtered out by `IsCandidateWindow`) so it's never mistaken for a
  trackable window; even if it briefly steals foreground, that's now
  harmless (no per-window history left to corrupt, unlike the original
  overlay-button bug this project already hit once with a similar
  non-activating popup).

**Implementation progress**, working through the milestones in the
dedicated plan drafted for this feature (real files, branch `alt_tab`):

- [x] **M0 — spiked both open technical questions with throwaway code**
  before committing to the design above.
  - (a) `WH_KEYBOARD_LL` suppression: **confirmed working end-to-end**,
    human-verified — a spike hook that beeped and updated its console
    title on every swallowed Tab-while-Alt was tested live: native
    Alt+Tab never appeared, no menu-bar flash on the target app. (First
    attempt appeared to fail with total silence; root cause turned out to
    be that the launch method used to start the spike process never
    actually produced a running process at all — nothing to do with the
    hook itself. Worth remembering: confirm a spawned GUI process is
    actually alive, via `Get-Process`/a log file it writes, before
    trusting a "nothing happened" result as a real negative.)
  - (b) DWM thumbnail on a plain (non-layered) `WS_POPUP` window: both
    `DwmRegisterThumbnail`/`DwmUpdateThumbnailProperties` calls returned
    `S_OK`. Visual confirmation (does a live preview actually render)
    still pending — the spike happened to capture the lock screen as its
    source window rather than a useful app, and the user wasn't at their
    computer to redo it with a better source. Not blocking further work;
    revisit before M4.
- [x] **M1 — `ActivationHistory`** (`src/windowtracking/ActivationHistory.h/.cpp`):
  pure MRU-list class, 6 unit tests, wired passively into
  `OnForegroundChanged`/`EVENT_OBJECT_DESTROY` in `main.cpp`. MRU order
  logged on every foreground change and confirmed correct in
  `%TEMP%\polish.log`.
- [x] **M2 (code) — `AltTabHook`** (`src/hook/AltTabHook.h/.cpp`): the
  real hook, dispatching via a private `WM_APP` message to
  `onCycle(bool)`/`onCommit()`/`onCancel()` callbacks — logging-only for
  now (`OnAltTabCycle`/`OnAltTabCommit`/`OnAltTabCancel` in `main.cpp`),
  no popup or candidate-list wiring yet. Builds clean, 9/9 unit tests
  pass, app starts and installs the hook without error.
  - [x] **First real-app test: native Alt+Tab correctly suppressed**,
    log confirmed clean `cycle forward` × N → `commit` sequences while
    holding Alt+Tab against the actual build (not just the M0(a) spike).
  - [x] **Real bug found and fixed: swallowing Alt-up (the original
    menu-flash mitigation from M0/M2's first version) left Windows'
    session-wide keyboard modifier state stuck believing Alt was still
    held** — surfaced as "letters started selecting menus" while typing
    in an unrelated app (VS Code), persisting even after `polish.exe`
    exited (OS/session-level state, not this process's to clean up on
    exit). Root cause: a low-level hook that swallows a *modifier*
    key-up prevents that event from ever reaching the layer that
    maintains synchronous keyboard state (`GetKeyState`/menu-mnemonic
    tracking/etc.) — the hook's own `LLKHF_ALTDOWN` read still works
    (it comes straight off the hardware event), but nothing else in the
    system ever finds out Alt went up. **Never swallow a modifier
    key-up.** Fixed by never swallowing Alt-up at all, and instead
    injecting a harmless dummy keystroke (`SendInput`, a bare Ctrl
    tap) right after the *first* swallowed Tab in a session — this
    breaks the "standalone Alt press" condition that triggers
    menu-mnemonic focus, without ever touching Alt itself, so Windows'
    own modifier-state tracking stays correctly in sync. See
    `AltTabHook.cpp`'s `InjectHarmlessKeystroke` for the implementation
    and full reasoning. As part of this fix, Escape (cancel) now also
    ends the session outright (previously left it open so a later
    Alt-up would still be swallowed — no longer applicable, and was
    causing a redundant `onCommit()` after `onCancel()` for the same
    gesture); a later Tab press while Alt is still held now correctly
    starts a fresh session instead.
  - [x] Rebuilt clean, 9/9 tests still pass. **Re-verified live and
    confirmed fixed**: native Alt+Tab stays suppressed, no menu-bar
    flash, and keyboard modifier state no longer gets stuck after
    exiting.
- [x] **M3+ pivot (2026-08-30): dimming overlay replaces the DWM-thumbnail
  popup plan entirely.** The user's call, made after seeing M2 work:
  instead of a thumbnail-grid popup (the original, more complex M3–M6
  plan below this note, now abandoned), highlight the actual candidate
  window in its real on-screen position and dim every other candidate,
  so "is this the window I want" is answered by literally looking at the
  real window, not by matching a small thumbnail to what you remember it
  looking like. This eliminates the DWM thumbnail API, the switcher
  popup's layout/DPI/monitor-placement questions, and most of what made
  this the biggest feature in the app — replaced by a much smaller
  design: `AltTabDimOverlay` (`src/hook/AltTabDimOverlay.h/.cpp`), a
  simple `WS_EX_LAYERED`/`WS_EX_TRANSPARENT`/`WS_EX_NOACTIVATE`/
  `WS_EX_TOPMOST` popup per candidate window, solid black at partial
  alpha (`SetLayeredWindowAttributes`, no DIB/premultiply pipeline
  needed — a uniform tint doesn't need per-pixel alpha), sized/positioned
  to exactly match its target's `GetWindowRect()`. Session state
  (candidate snapshot, highlight index, overlay pool) lives as plain
  globals in `main.cpp`, same pattern as `g_trackedWindow`.
  - [x] Basic dimming working — human-confirmed.
  - [x] **Real bug: highlighted window could still end up hidden behind
    another (e.g. maximized) window.** Dimming alone isn't enough if the
    highlighted window itself isn't actually visible. Fixed by making
    the highlighted window itself `HWND_TOPMOST` while highlighted
    (`SWP_NOACTIVATE` — purely visual, no focus change) and demoting it
    back (`HWND_NOTOPMOST`) the instant highlight moves off it or the
    session ends, so exactly one window is ever topmost at a time.
  - [x] **Real bug: promotion order.** Among windows marked
    `HWND_TOPMOST`, whichever gets that status *most recently* ends up
    frontmost. Since every dim overlay is also topmost, promoting the
    highlighted window before placing the other overlays in the same
    `ApplyAltTabDimming()` pass let a later overlay end up in front of
    it. Fixed by restructuring so the highlighted window's own promotion
    always happens last, strictly after every other overlay is placed.
  - [x] **Real bug: the committed window wasn't actually becoming
    active.** `SetForegroundWindow` from a background process is subject
    to Windows' foreground-lock heuristic and can be silently ignored.
    Fixed with the standard, widely-used workaround: inject a harmless
    dummy keystroke (`SendInput`, a bare Ctrl tap) immediately before the
    `SetForegroundWindow` call — resets whatever internal "did this
    process just handle real input" state that heuristic checks.
    Human-confirmed fixed.
  - [x] Diagnostic logging added for `SetWindowPos(HWND_TOPMOST)`
    failures, most likely cause being UIPI blocking cross-privilege
    manipulation of an elevated window (see `docs/LIMITATIONS.md` #1) —
    not yet actually triggered/confirmed in testing.
  - [x] **Real bug: dimming incorrectly affected windows in front of the
    dimmed one.** Each overlay was `WS_EX_TOPMOST`, sized to its target's
    rect — but that blindly covers the *rect*, not the target
    specifically, so any other window (candidate or not) stacked in
    front of the target on the real desktop got incorrectly dimmed too.
    Fixed by dropping `WS_EX_TOPMOST` entirely and instead inserting each
    overlay directly *above its own target only*, in the normal z-order
    (`GetWindow(target, GW_HWNDPREV)` for the insert-after handle, or
    `HWND_TOP` if nothing is above target) — anything that was already
    in front of the target now stays in front of its dim overlay too.
    Rebuilt clean, 9/9 tests pass. Human-confirmed no longer an issue in
    the 2-window case (no further complaint after this fix); superseded
    by the promote/demote fix below for the "behind another window" case.
  - [x] **Real bug: the highlighted window could still end up behind
    another (dimmed) candidate window,** even with the z-order fix above
    — `SWP_NOACTIVATE` alone doesn't reliably force DWM to fully
    recompute z-order for a window that's never actually activated.
    Fixed with a more robust, well-established technique: promote the
    highlighted window to `HWND_TOPMOST`, then *immediately* demote it
    back to `HWND_NOTOPMOST` — the brief topmost pulse forces it above
    everything, and the immediate demotion settles it at the front of the
    normal band instead of leaving it stuck topmost. Also simplified
    `EndAltTabSession` (nothing to demote anymore, since the highlighted
    window is never left topmost even transiently between cycles). Rebuilt
    clean, 9/9 tests pass, human-confirmed fixed.
  - [x] **Real bug, confirmed with log evidence: "Alt+Tab only shows 2
    apps" after minimizing down to 3 non-minimized windows.** The
    candidate-list diagnostic logging (added to investigate this)
    confirmed it directly: the MRU order had only 3 tracked windows total
    (2 real candidates + 1 correctly-filtered system window), meaning the
    3rd real app had simply never been foreground since Polish started
    tracking — the same known limitation the rest of the app already has
    (`docs/LIMITATIONS.md` #3), but here it silently made the *entire
    feature* miss windows rather than just delaying a sync. Fixed by
    rebuilding the candidate list from `EnumWindows` (every currently
    open, real, non-minimized window) rather than purely from
    `g_activationHistory` — windows Polish does have recency data for are
    still ordered by true MRU first; anything else falls back to
    `EnumWindows`'s own Z-order, appended after. Rebuilt clean, 9/9 tests
    pass; not yet re-verified live.
  - [ ] **Open, not yet diagnosed: user reports visual "shadows" left
    behind on a window after tabbing away from it.** Unclear yet whether
    this is a static artifact (e.g. DWM's drop-shadow/glow rendering
    slightly outside `GetWindowRect()`'s bounds, so the overlay doesn't
    cover it) or a transient one (a redraw/timing lag where old content
    briefly still shows through). Deferred until it can be described
    more precisely or reproduced with a screenshot — not fixing on a
    guess.
- [x] **Native Alt+Tab fallthrough when there are fewer than 2
  candidates.** Turned out not to be the "low priority, rare in
  practice" gap it was first noted as — human-reported as feeling
  *broken*, not just unhelpful: with 0 or 1 non-minimized windows open,
  the hook was still always swallowing Tab-while-Alt, eating the
  keystroke into total silence (no native switcher, no dim overlay,
  nothing). Fixed by giving `AltTabHook` a new
  `hasEligibleCandidates` callback (`AltTabHasEligibleCandidates` in
  `main.cpp`, which rebuilds `g_altTabCandidates` as a side effect),
  called **synchronously inside the hook callback** — the one deliberate
  exception to "the hook stays trivial," justified because `EnumWindows`
  plus cheap per-window checks is a bounded, no-UI operation nowhere
  near the timeout risk that popup/DWM work would be; the *rendering*
  work still stays deferred via the posted message. Returning false lets
  the keystroke fall through to native Alt+Tab completely untouched (also
  fixed a latent asymmetry: the Tab-up swallow wasn't gated on
  `sessionActive_`, so it would have swallowed the matching up even when
  the down had been allowed through). `OnAltTabCycle` no longer rebuilds
  the candidate list or bails out on its own — the hook guarantees it's
  already fresh with ≥2 entries by the time a new session reaches it.
  Rebuilt clean, 9/9 tests pass; not yet re-verified live.
- [ ] Live-updating the candidate list mid-session (currently a snapshot
  taken once at session start) — not started, noted as a known
  simplification, not urgent.
- [x] **UI tweak: make it obvious you're in Polish's Alt+Tab state, not
  just relying on relative brightness.** Dim alpha bumped from 140 to
  190/255 (noticeably darker). Added `AltTabHighlightBorder`
  (`src/hook/AltTabHighlightBorder.h/.cpp`) — an accent-blue glow drawn
  just outside the highlighted window's rect, as an active "this one"
  signal rather than only "the undimmed one." Rebuilt clean, 9/9 tests
  pass; not yet re-verified live.
  - [x] **Follow-up, user-requested redesign**: rounded rect (not sharp
    corners), ~1 inch thick, solid at the outer edge fading to fully
    transparent toward the inner edge (not a flat single-alpha frame).
    A gradient needs real per-pixel alpha, which
    `SetLayeredWindowAttributes`'s single constant alpha can't do, so
    this rewrites the class to reuse this project's own established
    rendering pipeline from its history (see the "Real bugs found and
    fixed" section further down): a top-down `CreateDIBSection`, a
    `Gdiplus::Bitmap` wrapping that DIB's memory directly (`Graphics(HDC)`
    does not reliably preserve alpha — confirmed the hard way once
    already in this app), a `Gdiplus::PathGradientBrush` (rounded-rect
    path, opaque surround color, transparent center color,
    `SetFocusScales` tuned so the fade completes within roughly the
    border's own thickness rather than fading all the way to the shape's
    geometric center), manual premultiply, then `UpdateLayeredWindow`
    with `ULW_ALPHA`. `SetWindowRgn`-based hard-edged clipping is gone
    entirely — per-pixel alpha reaching zero handles "never cover the
    target's content" more smoothly than a hard region cut ever could.
    `gdi32`/`gdiplus` added back to `polish_core`'s link libraries.
    One new build snag, not previously hit in this app: GDI+ headers
    need `IStream` from `<objidl.h>`, which the project-wide
    `WIN32_LEAN_AND_MEAN` define otherwise excludes from `<windows.h>` —
    fixed with an explicit include. Rebuilt clean, 9/9 tests pass.
  - [x] **Second follow-up**: positioned directly on the target's own
    rect (not inflated outward into the desktop margin around it) — the
    fade now goes inward into the window's own edge instead of projecting
    outside it, per explicit request. Simplified `ShowAroundTarget`
    accordingly (no more `InflateRect`). Rebuilt clean, 9/9 tests pass;
    not yet re-verified live.
  - [x] **Third follow-up**: thickness reduced to ~1/3 (32px @96 DPI,
    down from 96px). Also fixed a real, human-reported glitch while
    cycling: the glow visibly moved to the new target's origin *at the
    old target's size*, then snapped to the correct size a moment later.
    Root cause: position/size were set via a separate `SetWindowPos`
    call before the content-repainting `UpdateLayeredWindow` call, and
    `UpdateLayeredWindow` on a layered window stretches/clips whatever
    bitmap it last painted into the *current* window bounds until it's
    called again — so there was a real, visible gap where the window
    frame had already moved/resized but the painted content hadn't
    caught up yet. Fixed by moving position+size into the
    `UpdateLayeredWindow` call itself (its `pptDst`/`psize` params) so
    the move, resize, and repaint all happen as one atomic OS call;
    `SetWindowPos` now only handles topmost+visible
    (`SWP_NOMOVE | SWP_NOSIZE`). Rebuilt clean, 9/9 tests pass.
  - [x] **Fourth follow-up**: corner radius matched to Windows 11's own
    default window-corner rounding (8px @96 DPI, what VS Code and most
    native apps use — down from an arbitrary, more obviously-rounded
    32px). Fade band widened to 128px (4x), but reshaped from a linear
    gradient to a fast-falloff multi-stop curve
    (`PathGradientBrush::SetInterpolationColors`, 5 stops: solid at the
    edge, already mostly gone by 10% of the way in, fully transparent by
    45%, flat the rest of the way) — the extra width gives the falloff
    room to look soft and organic rather than like a visible band with
    edges of its own, without actually staying visible any wider than
    before. Rebuilt clean, 9/9 tests pass; not yet re-verified live.
- [x] **Escape hatch: Ctrl+Alt+Tab bypasses Polish entirely, native
  Windows Alt+Tab handles it instead.** Requested explicitly as a backup
  in case Polish's own switcher misbehaves. Tracked via a new
  `ctrlHeld_` (same observed-from-hook-events pattern as `shiftHeld_`)
  and `nativeHandoffActive_`, which stays true for the rest of the
  current Alt-hold once triggered so every subsequent Tab is *also* left
  untouched — otherwise releasing Ctrl mid-hold could make Polish start
  intercepting midway through native's own switcher session. Alt-up
  handling restructured to unconditionally reset `nativeHandoffActive_`
  regardless of which path (a real Polish session, a native handoff, or
  neither) the Alt-hold took. Rebuilt clean, 9/9 tests pass; not yet
  re-verified live. Not yet documented in README.md — holding off until
  the feature as a whole is done, not just this piece.
- [x] **Real bug: mouse clicks during a session reached whatever real
  window was underneath completely normally, leaving Polish's session
  state stale.** The dim overlays are `WS_EX_TRANSPARENT` on purpose, so
  nothing about them blocks a click — meaning the user could click
  close/minimize/maximize on a visible (dimmed or highlighted) window
  mid-session, changing its shape or making it stop existing while the
  highlight/dim visuals still pointed at it. Fixed by adding a second,
  dynamically installed/uninstalled `WH_MOUSE_LL` hook to `AltTabHook`
  (installed only while a session is open, not for the app's whole
  lifetime like the keyboard hook — mouse-move volume is far higher than
  keyboard, no reason to pay for that outside the brief window it's
  needed) that watches for any mouse button press and immediately
  **commits** (not cancels) with whatever's currently highlighted.
  Rebuilt clean, 9/9 tests pass.
  - [x] **Follow-up correction**: the click is now deliberately left
    untouched (not swallowed) rather than consumed by the commit — it
    reaches whatever's actually under the cursor completely normally,
    at the same time as ending the session. Requested explicitly: a
    single click should be able to both e.g. minimize some window and
    land Alt+Tab on whatever was highlighted, which don't have to be the
    same window. Rebuilt clean, 9/9 tests pass.
- [x] **Real bug: the previously-active window briefly still looked
  highlighted/undimmed right when a session started, before the real
  highlight (index 1, the previous window) visibly caught up.** Internal
  state was already correct from the first `ApplyAltTabDimming()` call
  (`g_altTabHighlightIndex` is set to 1 before it's ever called) — this
  was a rendering-latency issue, not a logic bug: the dim overlays and
  highlight border are created lazily (`CreateWindowExW` + first paint)
  on whichever session happens to need them first, and that creation
  cost was being paid synchronously in response to the user's actual
  first Tab press. Fixed by pre-creating the overlay pool and highlight
  border at app startup instead (`EnsureAltTabOverlayPoolSize`/
  `EnsureAltTabHighlightBorder` called once in `wWinMain`, sized from an
  initial `RebuildAltTabCandidates()`) — purely a head start for the
  common case, since the pool still grows safely later if more windows
  open than were open at startup. Rebuilt clean, 9/9 tests pass.
  - [ ] **Did not fix it — user re-confirmed the flash is still there.**
    Root cause still unconfirmed; not guessing again. Added real
    diagnostics instead: `AltTabHook::LastTabDetectedTick()` (a cheap
    `GetTickCount64()` captured the instant Tab-while-Alt is detected in
    the hook, no logging inside the hook itself) diffed against
    `GetTickCount64()` when `OnAltTabCycle` actually starts processing
    (message-queue latency), plus three more checkpoints through
    `ApplyAltTabDimming` (other-candidates' overlays, the highlighted
    window's promote/demote pulse, the highlight border's render) so the
    delay can be attributed to a specific stage instead of guessed at.
    Rebuilt clean, 9/9 tests pass.
  - [x] **The diagnostics found it**: message-queue delay was 0–15ms
    (negligible) and the other two stages were 0–16ms, but the highlight
    border's own render consistently dominated at 47–125ms — an order of
    magnitude slower than everything else combined. Root cause: the
    `PathGradientBrush` fill covered the *entire* window-sized rounded
    rect, even though the fast-falloff curve leaves almost all of that
    interior fully transparent — for a large/maximized target, GDI+ was
    shading up to millions of pixels it didn't need to. Fixed by clipping
    the fill to just the border band (`Gdiplus::Region` built from the
    outer path, `Exclude`d by an inner rect inset `thickness` pixels —
    the full thickness as a safety margin, not the tighter 45%-of-
    thickness point the gradient actually reaches zero at, so there's no
    risk of clipping through a still-fading pixel) before calling
    `FillPath` — GDI+ only rasterizes pixels inside the clip, so cost
    stays roughly constant regardless of target window size. Rebuilt
    clean, 9/9 tests pass.
  - [x] **User-confirmed: no longer seeing the flash.** Worth being
    honest about the actual evidence, though: re-measured timing after
    this fix still showed `highlightBorder` at 31–109ms, barely different
    from before the clip — so the clip optimization (still a legitimate,
    kept perf win) likely wasn't the deciding factor after all. Best
    explanation: the key-repeat fix (tracked separately, landed just
    before this) was probably the real fix here — before it, a single
    physical Tab press could trigger multiple rapid cycle events via OS
    auto-repeat, which would look exactly like "active window highlighted,
    then immediately jumps to the next one." Once repeat-cycling stopped,
    that illusion likely went away. Not chasing the render-time number
    further since there's no user-visible problem left to fix.
- [ ] **Open, not yet diagnosed: user reports the Windows Settings app
  sometimes appears to launch mid Alt+Tab, when it wasn't running
  before.** No repro steps yet, happened "several times." Leading
  hypothesis: a hidden/suspended UWP host window (Settings and other
  first-party UWP apps often stay resident even when the user believes
  they're closed) is passing `IsCandidateWindow`'s filter and getting
  committed to like any other candidate — `SetForegroundWindow` on it
  would make it visibly pop up, looking exactly like "it just launched."
  Added class names (not just titles) to the candidate-list dump
  specifically to catch this (watch for `ApplicationFrameHost`,
  `Windows.UI.Core.CoreWindow`, or similar in the log) — not yet
  confirmed either way.
- [x] **UX fix: holding Tab down was rapidly cycling through candidates
  via OS key-repeat, instead of staying on the current highlight until a
  genuine fresh press** (native Alt+Tab doesn't auto-cycle on repeat
  either). A low-level hook gets no repeat-count for a key the way a
  normal `WM_KEYDOWN`'s lParam would carry one, so this is tracked the
  same observed-from-hook-events way as `shiftHeld_`/`ctrlHeld_`: a new
  `tabPhysicallyDown_`, true from a genuine Tab-down until its matching
  Tab-up, with repeated Tab-down events while it's already true
  recognized as OS repeat and ignored (still swallowed if a session is
  active, so the repeat doesn't leak through to whatever's behind it).
  Tab-up handling restructured to unconditionally reset
  `tabPhysicallyDown_` (not gated on `altHeld`/`sessionActive_`) so it
  can never get stuck true. Rebuilt clean, 9/9 tests pass; not yet
  re-verified live.

### Alt+Tab improvements, round 2 (branch `alt_tab_improvements`, off `main`)

Full plan at `C:\Users\david\.claude\plans\i-d-like-to-work-moonlit-flute.md`:
live-updating the candidate list mid-session, a thinner/solid highlight
border, a combined transparent list panel (active windows + a minimized
section below), and per-row minimize/restore-toggle and close buttons on
the highlighted row.

- [x] **M1 done: candidate list now rebuilds on every cycle, not just once
  at session start.** `OnAltTabCycle` calls `RebuildAltTabCandidates()`
  unconditionally (previously only `AltTabHasEligibleCandidates` did, and
  only on a session's first Tab) -- safe since this already runs off the
  hook thread via the existing `PostMessageW` hop, same reasoning as that
  call's own hook-thread exception. Handles three new hazards a live
  rebuild introduces: re-locates the highlighted window by identity (not
  index) after each rebuild, falling back to a clamped index if it closed;
  hides any dim overlay whose index fell out of range on a shrink (the
  pool only ever grows); ends the session cleanly if the count drops below
  2 mid-session instead of dividing/modding by a degenerate count. Rebuilt
  clean, all 45 tests pass (no existing test touches this path).
  **Live-verified** via a scripted SendInput harness (real Explorer
  windows, `WM_CLOSE`'d mid-session): closing a non-highlighted candidate
  mid-session rebuilt and re-cycled correctly; closing the *currently
  highlighted* candidate specifically (the harder case) correctly
  triggered the clamped-fallback path with no crash, a sane next highlight
  (confirmed against the log's index math by hand), and a clean commit
  afterward. (Aside, not a Polish bug: the test harness's own first attempt
  silently injected nothing at all -- a P/Invoke `INPUT` struct missing
  the `MOUSEINPUT` union member undersized it below the OS's expected
  `sizeof(INPUT)`, which makes `SendInput` fail its size check and return
  0. `GetAsyncKeyState` confirmed Alt genuinely wasn't registering as held
  before the struct fix -- worth remembering for any future scripted
  input-injection test in this repo.)
- [x] **M2 done: highlight border is now a thin, solid (fully opaque)
  ring instead of a soft gradient glow.** `AltTabHighlightBorder.cpp` kept
  the whole DIB + GDI+ + manual-premultiply + `UpdateLayeredWindow`
  pipeline (still needed for antialiased rounded corners on a hollow
  shape) but replaced `PathGradientBrush`/`SetInterpolationColors`/
  `SetFocusScales` with a flat `Gdiplus::SolidBrush`, and turned the old
  "clip out the interior" step from a pure perf optimization into the
  actual mechanism that carves the ring: excludes an inner rounded-rect
  path (via the same `BuildRoundedRectPath` helper, now parameterized with
  an offset so it can build an inset copy of itself) rather than the old
  plain axis-aligned `RectF`, which would have shown as a wrong-shaped
  inner corner once the fill went opaque. `thickness` changed meaning from
  a 128px fade zone to the ring's real visible width (3px logical,
  DPI-scaled); added a clamp (`min(thickness, width/2, height/2)`) so a
  target smaller than 2x the ring width never gets fully painted over.
  Rebuilt clean, all 45 tests pass. **Live-verified**: a screenshot taken
  mid-session shows a thin, solid accent-blue ring around the highlighted
  window with no visible fade, and the existing `highlightBorder={}ms`
  timing log dropped to 16ms (previously 31-125ms with the gradient),
  confirming no perf regression.
  - [x] **Real bug, fixed: the ring was invisible on a maximized/
    full-screen window.** `GetWindowRect` includes the modern invisible
    resize border, which Windows deliberately hangs a few px *off* the
    monitor's edges for a maximized window (confirmed directly: a
    maximized test window's `GetWindowRect` was `(-7,-7)-(1446,918)` on a
    1440x900-ish work area) so the window's actually-visible edge lines up
    with the screen edge -- drawing a 3px ring right at that raw rect's
    edge put nearly all of it off-screen. Fixed with
    `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` instead (falling
    back to `GetWindowRect` if it fails), which gives the tighter,
    actually-visible rect. Not a conflict with this app's other, unrelated
    GetWindowRect/extended-frame-bounds gotcha (restore-position-sync
    must never round-trip between the two) -- this is a single fresh
    query for a one-off visual rect, never stored or mixed with a
    GetWindowRect value from another code path. Verified via a compiled
    spike harness (this project's established methodology) against a
    real maximized Explorer window: screenshot confirms the ring now
    renders exactly at the screen's visible edge. Rebuilt clean, all 45
    tests pass.
  - [x] **Follow-up, human-reported: the ring still visibly got chopped
    off right at the screen's four corners on a maximized/full-screen
    target.** Root cause: this device physically rounds the display
    panel's own corners (some Surface models do this), with a
    noticeably larger radius than the ~8px window-corner radius the ring
    normally uses -- there's no documented API to query that hardware
    radius, so a thin ring drawn with the small window radius gets
    visibly clipped by it. Fixed by giving `BuildRoundedRectPath` (and
    the outer/inner path calculations in `ShowAroundTarget`)
    independent per-corner radii instead of one uniform value: for each
    corner, `MonitorFromWindow`/`GetMonitorInfoW` determines whether
    *both* of that corner's edges are flush (within a 2px tolerance)
    against the monitor's own physical `rcMonitor` bounds -- if so, that
    corner uses a larger, tuned `screenRadius` (28px logical @96dpi)
    instead of the normal 8px. This correctly generalizes past simple
    "maximized vs. not": a normal maximized window (taskbar at the
    bottom) gets the screen radius on its top-left/top-right corners
    only (flush against the screen there) while its bottom two corners
    -- inset by the taskbar -- keep the normal small radius; a true
    borderless-fullscreen window gets all four. `screenRadius` is a
    tuned constant, not a queried value (flagged the same way as
    `thickness`/the base `radius` already are in this class -- needs
    live confirmation, may need adjusting per-device). Rebuilt clean,
    all 45 tests pass. **Live-verified** via the same compiled spike
    harness against a real maximized Explorer window: screenshot
    confirms a properly large, smooth rounded corner at the screen edge
    instead of a clipped one.

**Superseded M3–M6 plan (DWM-thumbnail popup), kept for reference, not being built:**
M3 — `AltTabSwitcherWindow` placeholder-chrome skeleton; M4 — real DWM
thumbnails replace the placeholders; M5 — commit/cancel wired to
`SetForegroundWindow`; M6 — labels, DPI/multi-monitor correctness, edge
cases.

### 2. Settings menu: per-feature toggles, autostart, About

User-requested: a way to individually disable each feature, a "load at
startup" option, and an About item advertising the author as available
for hire.

- [x] `src/settings/Settings.h/.cpp` — `restoreSyncEnabled`/
  `altTabEnabled`, persisted to `HKCU\Software\Polish` (registry, not a
  config file — no other reason this app would need the filesystem, and
  the registry gives atomic per-value read/write for free). Autostart is
  deliberately *not* one of these persisted flags — `IsStartAtLoginEnabled`
  queries the `HKCU\...\Run` key's own presence live instead, so a user
  who removes it via Windows' own Startup Apps settings doesn't leave
  Polish's cached idea of the setting stale.
- [x] `TrayIcon` reworked from a hardcoded Exit-only menu to a
  `populateMenu(HMENU)` callback the caller fills in fresh each time the
  menu opens (so checkbox state, especially Start with Windows, is always
  current) plus a generic `onCommand(UINT)` callback — `TrayIcon` now
  owns none of the menu content or command IDs, purely the tray
  icon/menu mechanics. `kExitCommandId` removed; `main.cpp` owns Exit
  like every other item now.
- [x] Menu: **Restore remembers Snap position** (relabeled from "Restore-
  position sync" — the internal mechanism name didn't mean anything out
  of context) and **Alt+Tab (skip minimized)** checkboxes, **Start with
  Windows** checkbox, the About/hire-me item, **Exit**. Toggling a
  feature checkbox gates the
  actual behavior at its existing single choke point — `SyncRestorePlacement`
  returns immediately if `restoreSyncEnabled` is false;
  `AltTabHasEligibleCandidates` returns false immediately if
  `altTabEnabled` is false, which (already-built machinery) means native
  Alt+Tab just runs untouched rather than needing to
  install/uninstall the hook itself.
- [x] Menu item itself carries the pitch, not just its destination —
  the actual name, so it's visible even to someone who never clicks:
  **"By David Lenihan. Hire me!"**, opens his LinkedIn
  profile (`https://www.linkedin.com/in/davidlenihan/`) via
  `ShellExecuteW`.
- [x] Tray icon hover tooltip reworded to a tagline: **"Polish - Add fit
  and finish to Windows"** (was "Polish (running) - keeps Snap and
  Maximize/Restore in sync", which described only one feature and read
  as an internal status string, not a pitch).
- [x] Rebuilt clean, 9/9 tests pass, clean startup confirmed (settings
  load with correct first-run defaults when the registry key doesn't
  exist yet); not yet re-verified live for the actual toggle behavior.
- [ ] **Known gap: `IsStartAtLoginEnabled` only checks whether the Run
  key value exists, not Windows' separate `StartupApproved` state.**
  Task Manager's own Startup Apps tab can disable an entry without
  removing it from the Run key — if the user does that, our checkbox
  would still show checked even though Windows won't actually run it at
  login. Not handled; noted as a known limitation, not urgent.

### 3. Other backlog items

- Taskbar: Make hover show windows in actual position on screen so you can identify
  a window spacially. Minimized windows would be a list without any thumbnail
- Show fullscreen animation when you copy to clipboard
- Desktop replacement that is more useful: calendar, weather...TBD  
- Full multi-window tracking (sync every window, not just the
  foreground one) — `WinEventHookManager`, a tracked-window set seeded
  via `EnumWindows`, and a periodic reconciliation sweep.
- Exclusion list by process name (global enable/disable + autostart
  landed above; per-process exclusion did not).
- "Restart as Administrator" tray item, so elevated windows (currently
  invisible to Polish — see limitations #1) can be covered too.
- Full `docs/LIMITATIONS.md` pass and manual test matrix once the above
  land.

### 4. Window groups (tab and tile) — shipped, merged to `main` (2026-09-04)

Was branch `containers`; merged into `main` via fast-forward on
2026-09-04 (27 commits) and pushed to `origin/main`. The `containers`
branch itself is stale now (fully contained in `main`, nothing left to
land from it) — work from `main` going forward. Still-open items for
this feature are captured below and in "Future ideas / backlog" near
the end of this file, not on a separate branch.

Replaces Windows' old "Cascade windows" taskbar option: a group brings
multiple real windows together, switchable via tabs (like browser tabs)
or shown simultaneously as tiles. Full design plan (research findings,
resolved UX decisions, milestones) at
`C:\Users\david\.claude\plans\i-want-to-make-compressed-dusk.md` as of
2026-08-31 — summary here, that file has the detail. (Originally called
"containers" throughout planning; renamed to "groups" per user request
on 2026-08-31 — the branch name `containers` was kept as-is, only the
feature's own terminology changed.)

**Resolved design decisions:**

- **Superseded 2026-09-02 — see "Architecture reversal" below.** The
  "never `SetParent`" decision below was correct for the goal it was
  evaluated against at the time, but the user later clarified the
  feature's actual goal (collapsing many windows, e.g. 20 Notepads,
  into a handful of Alt+Tab-reachable groups) in a way that changes the
  tradeoff entirely — kept here for the record, not because it's still
  the design.
- **~~Never `SetParent` across process boundaries — reposition-only.~~**
  Researched, not assumed: cross-process `SetParent` attaches the two
  threads' input queues (a hang in the member's process hangs Polish's
  UI thread too — Raymond Chen), is silently blocked by UIPI for
  elevated members (same wall already hit for `SetWindowPos`/
  `SetForegroundWindow`, `docs/LIMITATIONS.md` #1), and on Windows 11
  with GPU compositing specifically DWM has been reported to keep
  rendering a reparented window as its own independent composited
  surface regardless of its new logical parent — the visual containment
  the feature depends on may just not happen. Actively maintained tools
  today (GlazeWM, komorebi) don't reparent either; TidyTabs (closest
  prior art) does and users report exactly the resulting instability. So
  a group is pure coordinate/z-order choreography over untouched,
  independent top-level windows — the same technique already proven by
  `AltTabDimOverlay`/`AltTabHighlightBorder`.
- **The group is a regular, taskbar-visible, Alt+Tab-visible application
  window — not a special Polish-owned overlay.** Revised mid-planning
  from an earlier "group excluded from Alt+Tab" idea; the user corrected
  this explicitly ("it is a regular .exe"). This part of the decision
  still stands post-reversal. **The "member windows also show up in
  Alt+Tab independently" half of this bullet does not** — see
  "Architecture reversal" below: reparented members are children, and
  `EnumWindows`-based candidate lists (Alt+Tab, the picker) structurally
  can't see children at all, which turned out to be exactly the point
  once the actual goal (collapsing many windows into few Alt+Tab
  entries) was clarified.
- **Creation**: explicit "New Group" action (tray menu item and a
  **Win+Alt+G** global hotkey) → picker of currently open windows to
  populate it. **Add/remove**: picker-based for v1; drag-in/drag-out is
  the desired end state but a stretch goal, not guaranteed.
  **Persistence**: groups persist across restarts with best-effort
  reattachment by process name + title (HWNDs never survive a restart) —
  user chose this over simpler in-memory-only, knowing matches will
  sometimes be wrong. **Group dragging**: dragging the chrome moves every
  member together. **Mode**: one group can switch between tab/tile at
  runtime (not two separate types). **Lock/unlock** and **nesting** (a
  group can contain other groups) were also requested.
- **v1 scope is deliberately smaller than all of the above**: a
  single-level, single-mode-per-instance group, no persistence.
  Mode-switching, lock/unlock, nesting, and persistence are explicit
  post-v1 milestones (M7+), each needing their own design pass —
  building everything at once would repeat a mistake this project
  already made once (the original Alt+Tab plan fully designed a
  DWM-thumbnail popup that was abandoned before half of it was built).

**Milestones** (M0–M6, each independently verified before the next —
see the plan file for full detail): M0 spikes that pure `SetWindowPos`
repositioning (no reparenting) looks correct for 2-3 real apps including
one packaged/UWP app; M1 `GroupState` + unit tests; M2 the creation/
picker flow; M3 static chrome rendering, verified it *does* appear in
Alt+Tab; M4 real tab switching (click and via-Alt+Tab), group-drag; M5
the tile variant; M6 edge cases (member closed, elevated member
excluded, multi-monitor/DPI, cleanup on group destroy).

- [x] **M0 done** (mixed-DPI monitor case deferred to before M6, not
  blocking). Spiked pure `SetWindowPos` repositioning against three real
  running apps (Notepad, Settings — a real UWP/packaged app via
  `ApplicationFrameWindow` — and VS Code), all landed correctly in target
  slots with no `SetParent` anywhere. Found and fixed a real bug: two
  were maximized, and `SetWindowPos` silently no-ops on size/position
  while a window is still maximized — `ShowWindow(SW_RESTORE)` first is
  now a confirmed requirement for `GroupManager`, not just a spike
  detail. Human-confirmed group-dragging doesn't need `SetParent` either
  — mechanism (hook the chrome's own `WM_MOVING`, re-drive each member's
  `SetWindowPos` in lockstep) added to the plan file.
- [x] **M1 done.** `src/windowtracking/GroupState.h/.cpp` — pure
  state class (ordered membership, active index, tab/tile mode; member
  storage already shaped for later nesting via a tagged
  `GroupMemberKind`, without implementing recursion yet) +
  `tests/GroupStateTests.cpp` (14 tests, all 24 project tests pass).
- [x] **M2 done.** "New Group" creation flow, both triggers wired to the
  same `TriggerNewGroup(HWND owner)` in `main.cpp`: a new tray menu item
  and a `RegisterHotKey`/`WM_HOTKEY` Win+Alt+G hotkey (no existing
  hotkey infrastructure to reuse — `HotkeyManager` was fully deleted
  earlier in the project's history — a single `RegisterHotKey` call is
  simple enough not to need one of its own). New files:
  `src/windowtracking/WindowFilters.h/.cpp` (candidate-window filter
  extracted out of `main.cpp`'s former local `IsCandidateWindow` so both
  Alt+Tab and the picker share it, plus a new `IsElevatedWindow` —
  compares the target process's token elevation against this one via
  `GetTokenInformation(TokenElevation)`, used to exclude elevated
  windows from the picker per the plan's "Bug categories to expect"
  section, rather than offering something `SetWindowPos` would later
  silently fail on); `src/hook/GroupPickerWindow.h/.cpp` (the picker
  itself — not a real Win32 dialog/.rc template, a plain `WS_POPUP`
  window with a checkbox-enabled `SysListView32` (Common Controls v6,
  already declared in `app.manifest`) and its own nested message loop
  while shown, same from-scratch-window spirit as `AltTabDimOverlay`/
  `AltTabHighlightBorder`); `src/windowtracking/GroupManager.h/.cpp`
  (owns the set of created `GroupState`s, `CreateGroup`/`FindGroup`) +
  `tests/GroupManagerTests.cpp` (5 tests, all 29 project tests pass).
  Live-verified end to end (not just build-verified) via a scripted
  smoke test — `PostMessage`d the exact `WM_COMMAND` a real tray click
  sends, confirmed the picker appeared populated with real candidate
  windows, `BM_CLICK`'d Create Group, and read `%TEMP%\polish.log` to
  confirm `GroupManager::CreateGroup` ran; separately verified the
  cancel path (`WM_CLOSE`) logs a cancellation and creates no group.
  Two real bugs found and fixed along the way, neither specific to
  groups:
  - **The Win+Alt+G hotkey fails to register on this dev machine**
    (`RegisterHotKey` returns `GetLastError()=1409`,
    `ERROR_HOTKEY_ALREADY_REGISTERED`) — this machine already has
    something claiming a broad range of `Win+Alt+<letter>` combos, the
    exact same quirk noted below in the Win32-gotchas history from the
    abandoned `Win+Alt+T` toggle hotkey. Not a code bug and not
    blocking: the tray menu item calls the identical `TriggerNewGroup`
    path regardless of whether the hotkey claimed successfully, so the
    feature works either way — but the hotkey itself should be
    re-verified on a clean machine (or after finding/disabling
    whatever's claiming it here) before considering it done.
  - **Real, general logging bug, not just a group-picker cosmetic
    issue**: `LogDebug` (`src/util/Logging.h`) wrote to
    `%TEMP%\polish.log` via a plain `std::wofstream` with no explicit
    codecvt facet — the "C" locale's default narrow conversion can't
    represent any non-ASCII character, and silently truncates the rest
    of that line (including its trailing newline) the instant it hits
    one, corrupting the log stream for every subsequent line too (they
    all run together). First surfaced by the picker window's own title
    (an em dash), but this would have hit *any* real window title
    containing non-ASCII text (accented names, non-Latin scripts,
    emoji) — a real gap, not something specific to this feature. Fixed
    by converting to UTF-8 (`WideCharToMultiByte`) and writing via a
    narrow `std::ofstream` instead. Separately, MSVC itself needed
    `/utf-8` added to `CMakeLists.txt`'s compile options (both
    `polish_core` and `polish`) — without it, a non-ASCII character
    inside a wide string literal in a BOM-less source file is
    mis-decoded using the system codepage at *compile* time, no warning
    or error, silently baking corrupted text into the binary.
  - (Also: `FindWindow`/`FindWindowEx` from PowerShell/.NET P/Invoke
    treats a `$null` window-title argument as an empty string, not a
    true null pointer, so it only matches windows with an empty title —
    a testing-tooling gotcha, not a Polish bug, but worth remembering
    for future scripted verification: pass the real title, or filter by
    class name via `EnumWindows` instead.)
- [x] **M3 done.** `src/hook/GroupChromeWindow.h/.cpp`: a real, normal
  top-level `WS_OVERLAPPEDWINDOW` (no `WS_EX_TOOLWINDOW`) that renders a
  static tab strip -- one rectangular tab per member, titled from
  `GetWindowTextW`, plain GDI (`FillRect`/`Rectangle`/`DrawTextW`), DPI-
  scaled via `GetDpiForWindow`. Deliberately **not** using the DIB +
  GDI+ + premultiply + `UpdateLayeredWindow` pipeline the plan
  originally called out for this -- that pipeline exists to solve a
  translucent-gradient alpha problem (see `AltTabHighlightBorder`) that
  a flat, fully opaque tab strip doesn't have; reusing it here would've
  been unnecessary complexity, not the "reuse a proven technique" win it
  was for the highlight border. `TriggerNewGroup` (`main.cpp`) now
  creates one `GroupChromeWindow` per created group (titled member
  titles captured at creation time -- static, no live updates yet) and
  keeps it in a new `g_groupChromeWindows` map keyed by `GroupId`,
  cleaned up on app exit.
  Live-verified, not just build-verified: the same scripted flow as M2
  (tray-menu `WM_COMMAND` → picker → `BM_CLICK` Create Group), then
  checked the resulting chrome window directly against
  `IsCandidateWindow`'s exact filter (`IsWindowVisible`, `!IsIconic`, no
  `GW_OWNER`, no `WS_EX_TOOLWINDOW`, has `WS_CAPTION`) -- all pass, and
  `%TEMP%\polish.log` independently confirms it: creating the chrome
  window fired a real `EVENT_SYSTEM_FOREGROUND`, and `OnForegroundChanged`
  logged `candidate=true` and added it to the MRU order exactly like any
  other real application window, with zero special-casing anywhere in
  that path -- concretely proving the "no special-casing needed"
  Alt+Tab-integration claim from the plan, not just asserting it.
- [x] **M4 done.** Real tab switching, member positioning, group-drag,
  and foreign-activation sync, all wired in `main.cpp`:
  - `src/windowtracking/WindowZOrder.h/.cpp` — `PromoteWindowToFront`,
    the promote-then-immediately-demote `HWND_TOPMOST` pulse extracted
    out of `ApplyAltTabDimming` (which now calls it too, behavior
    unchanged) so groups and Alt+Tab share one implementation.
  - `GroupManager::ApplyLayout(group, contentRect)` — the one path
    every reflow goes through (initial layout, a tab click, a
    group-drag, or an externally-activated member): restores any
    still-maximized member first (`ShowWindow(SW_RESTORE)`, confirmed
    required by M0), `SetWindowPos`s every member into `contentRect`
    (in tab mode every member shares the identical rect, so a
    group-drag is just re-applying this with a shifted rect -- no
    per-member offset math needed for v1), then promotes the active
    member via `PromoteWindowToFront`.
  - `GroupChromeWindow` gained active-tab highlighting (accent-blue
    fill vs. the earlier flat gray for every tab), `WM_LBUTTONDOWN`
    hit-testing against the tab rects (`onTabClicked_` callback), and
    `WM_WINDOWPOSCHANGING` handling (`onMoved_` callback) for
    group-drag. The move handling needed real care: at
    `WM_WINDOWPOSCHANGING` time the window hasn't actually moved yet,
    so the callback computes the *proposed* content rect directly from
    the `WINDOWPOS` struct's `x`/`y`/`cx`/`cy`, correctly accounting for
    the offset between the window's outer (non-client) rect and its
    client area (title bar + borders) -- an earlier version of this
    naively assumed the client area started right at the outer rect's
    edge, which ignored the title bar entirely and would have
    misplaced every member during a live drag. That offset is measured
    once from the window's current (still pre-move at that point)
    state and re-applied to the proposed position.
  - `main.cpp`: `ReflowGroupTo(id, contentRect)` (the single call site
    all three triggers funnel through), `ActivateGroupTab(id, index)`
    (tab click -- switches `GroupState`'s active index, the chrome's
    highlight, reflows, *and* actually calls `SetForegroundWindow` on
    the newly active member -- unlike Alt+Tab's highlight-preview
    promote, a tab click is a deliberate "switch to this" action, not
    a preview), and `SyncGroupFromForeground(hwnd)` (called
    unconditionally from `OnForegroundChanged` for every foreground
    change; no-op unless `hwnd` is a group member, in which case it
    updates that group's active index/chrome highlight and reflows,
    but deliberately does *not* call `SetForegroundWindow` -- hwnd is
    already foreground, that's what triggered it). The
    `SetForegroundWindow`-from-a-background-process workaround (a
    harmless injected Ctrl keystroke) was already duplicated once
    between `AltTabHook` and `OnAltTabCommit`; adding a third copy for
    `ActivateGroupTab` was one too many, so it's now a shared local
    `InjectHarmlessCtrlKeystroke()` used by both `OnAltTabCommit` and
    `ActivateGroupTab`.
  Verified with a compiled harness linked directly against
  `polish_core.lib` (same spike methodology as M0, not just build-
  verified) rather than fighting cross-process `SysListView32` checkbox
  automation from PowerShell (confirmed impractical -- `LVM_SETITEMSTATE`
  needs a pointer valid in the *target* process, which a plain
  cross-process `SendMessage` can't marshal). Against two real, running
  Notepad windows and a real `GroupChromeWindow`: (1) initial
  `ApplyLayout` positions both members into the content rect and
  promotes the active one to the front of the real desktop z-order
  (verified by walking `GW_HWNDNEXT`, not just trusting a return value);
  (2) switching the active index and re-applying promotes the other
  member instead; (3) a shifted content rect (simulating a drag) moves
  both members to follow; (4) a synthetic `WM_LBUTTONDOWN` at a computed
  tab coordinate correctly fires `onTabClicked_` with the right index;
  (5) moving the chrome window via `SetWindowPos` fires `onMoved_` with
  a proposed content rect that exactly matches
  `ContentRectInScreenCoords()` once the move actually completes --
  confirming the outer/client-offset fix above is actually correct, not
  just plausible-looking. All 5 checks pass.
- [x] **M5 done.** The tile variant, built as a mode flag through the
  existing pieces rather than a parallel implementation, exactly as
  planned:
  - `GroupPickerWindow` gained a Tab/Tile radio-button choice (v1 has no
    runtime mode switching -- M7+ -- so the mode is picked once, at
    creation time; Tab is the default, matching `GroupMode`'s own
    default). `ShowModal` now returns a `GroupPickerResult{windows,
    mode}` instead of a bare window list.
  - `GroupChromeWindow::Show` takes a `GroupMode`. In Tile mode the
    header renders as a plain label ("Group (N window(s), tiled)") with
    no clickable tabs -- `ComputeTabRects` returns empty, so
    `WM_LBUTTONDOWN` hit-testing naturally no-ops, no special-casing
    needed there.
  - `GroupManager::ApplyLayout` now branches on `group.Mode()`:
    `ApplyTabLayout` is the unchanged M4 behavior; `ApplyTileLayout`
    divides `contentRect` into a roughly square grid (`cols =
    ceil(sqrt(n))`, `rows = ceil(n/cols)`) and positions each member
    into its own slot, computed from `left + col * width / cols` (next
    boundary, not `left + col*slotWidth`) so integer-division remainder
    pixels don't accumulate into a gap/overlap at the grid's far edge.
    No z-order promotion in tile mode -- members don't overlap, so
    there's nothing to bring to front.
  Verified by extending the same compiled M0/M4-style harness: a
  3-member Tile group's members land in the exact computed grid-slot
  rects (positions matched exactly on the first attempt). Sizes
  initially did *not* match at a smaller content rect (1000x800, 500x400
  slots) -- not a bug in the layout math (every slot's `left`/`top` was
  still exactly correct), but a **real, confirmed constraint worth
  keeping in mind for M6's edge-case pass**: Notepad silently refused to
  shrink below its own declared minimum tracking size
  (`WM_GETMINMAXINFO`), clamping the actual window size wider/taller
  than the requested slot. Re-verified with a larger content rect
  (1600x1200, 800x600 slots, comfortably above that minimum) and all
  three slots matched exactly.
- [x] **Real user-reported bug, fixed: a member with a large minimum
  window size (Outlook) overflowed visibly outside a small group.**
  Direct confirmation of the M5 finding above, hit through actual usage
  rather than just the spike: `SetWindowPos` silently clamps to a
  window's own declared minimum tracking size instead of failing, and
  the group had no way to notice or react, so the member just visually
  spilled past the chrome's edge. Fixed properly rather than just
  documented as a limitation:
  - `GroupManager::ApplyLayout` (and both mode-specific helpers) now
    return a `SIZE` -- the smallest content area that would fit every
    member without any of them being clamped, computed from each
    member's *actual* post-`SetWindowPos` `GetWindowRect()`, not the
    requested rect. Tab mode: the max actual width/height across all
    members (they all share one rect). Tile mode: a proper per-column/
    per-row max, like HTML table auto-layout -- one oversized member
    only grows its own column/row, not the whole grid uniformly.
  - `GroupChromeWindow::GrowContentAreaTo(minContentSize)` resizes the
    chrome (top-left held fixed) to be at least that big, measuring the
    outer-rect-to-content-area offset the same way the
    `WM_WINDOWPOSCHANGING` handler already does, for the same reason
    (the client area doesn't start at the window's outer edge).
  - `main.cpp`'s `ReflowGroupTo` -- the single funnel every trigger
    (initial layout, tab click, group-drag, external activation) already
    went through -- now checks `ApplyLayout`'s returned size against
    what it asked for, and if larger, grows the chrome and re-applies
    layout once. No loop: growth is a one-shot correction, not a retry
    cycle.
  Verified by extending the M0/M4/M5 compiled harness (Test 7): forced a
  chrome down to a content size smaller than Notepad's own minimum
  (already established at ~664x404 by the M5 test), ran the exact
  detect-and-grow logic `ReflowGroupTo` uses, and confirmed the member
  ends up **exactly** contained in the grown content rect -- zero
  overflow in any direction, not just "smaller than before." All 7
  spike checks pass (the 6 from M4/M5 plus this one).
  Still an accepted gap, not chased further for v1: **a tile group
  where *multiple* members in the same row/column each have large,
  different minimums** can still grow larger than the screen/monitor,
  since growth doesn't currently clamp to monitor work-area bounds --
  carried into M6's multi-monitor edge-case pass below rather than
  guessed at now.
- [ ] M6 remaining: a member closed while backgrounded (tab) and while
  tiled (reflow both); an elevated window excluded from the picker as
  designed (already true since M2, re-verify here); multi-monitor/
  mixed-DPI member (including the mixed-DPI case deferred since M0) and
  the grow-to-fit-vs-monitor-bounds gap noted above. ("Chrome window
  destroyed leaves every member back in normal independent state" — the
  other M6 item originally listed here — is now done, below; it became
  a hard safety requirement rather than a nice-to-have once reparenting
  landed.)

### Architecture reversal (2026-09-02): real `SetParent` containment, not reposition-only

Live use surfaced a real gap the reposition-only design didn't address:
nothing stopped a member window from just being dragged away by its own
title bar, since it was always a fully independent top-level window —
Polish only ever reacted to drops, never prevented an in-progress drag.
The user asked directly why not just use real `SetParent`, which
prompted re-examining the original "never `SetParent`" decision rather
than patching around it.

**The real tradeoff, once actually laid out**: `EnumWindows` — the API
Polish's own Alt+Tab and the picker both depend on — does not enumerate
child windows at all. So reparenting a member for true containment and
keeping it individually reachable via Alt+Tab are mutually exclusive at
the Win32 level, not a matter of implementation care. This seemed like
a blocker at first. It wasn't: the user clarified the feature's actual
purpose is collapsing many windows (their own example: 20 Notepads)
into a handful of Alt+Tab-reachable *groups* — losing individual
Alt+Tab-reachability for members is exactly the point, not a cost.
Once that was clear, real `SetParent` was the obviously correct choice,
and this is now a deliberate, informed reversal of the earlier decision
— not the DWM-compositing/UIPI/input-queue risks turning out to be
overblown (they're still real and still relevant to any *other* feature
that might consider reparenting in this codebase).

**What changed:**

- **`src/windowtracking/WindowReparenting.h/.cpp`** (new) —
  `ReparentIntoGroup(hwnd, newParent)`: strips the member's own frame
  (`WS_CAPTION`/`WS_THICKFRAME`/`WS_SYSMENU`/min/max boxes — the
  group's chrome provides that context now) and its `WS_POPUP` bit,
  adds `WS_CHILD`, calls `SetParent`, forces a `SWP_FRAMECHANGED`
  recompute. `RestoreTopLevel(hwnd, backup)` reverses it exactly from
  the saved pre-change style. **Confirmed empirically, not just
  theorized**: a child window left parented to a chrome that gets
  destroyed *is* destroyed too (the exact hazard this API's own
  comments warn about) — but the destruction is asynchronous for a
  cross-process child (observed up to ~1s after the parent's
  `DestroyWindow` call returns, not synchronous within it), which is
  exactly why release must happen synchronously and *before*
  `DestroyWindow` is ever called, not "soon after."
- **`GroupManager`** — `ApplyLayout` now takes the chrome's `HWND` and
  reparents any not-yet-reparented member on first layout
  (`EnsureReparented`, tracked via a `std::map<HWND, ReparentBackup>`).
  Positions members in **client-relative coordinates** now (a child's
  `SetWindowPos` x/y are relative to its parent's client origin, not
  the screen — a fundamental coordinate-system change from the old
  design). Tab mode no longer needs the promote/demote `HWND_TOPMOST`
  z-order pulse at all — that trick was for independent top-level
  windows; true children are simply shown (`SW_SHOW`) or hidden
  (`SW_HIDE`), unambiguous and instant. New `ReleaseGroup`/
  `ReleaseMember` restore members to top-level.
- **`GroupChromeWindow`** — the `WM_WINDOWPOSCHANGING`-based "follow
  the drag" mechanism (`SetOnMoved`) is gone entirely: children move
  for free when their parent moves, no callback needed. Replaced by
  `SetOnResized` (`WM_SIZE` — a resize still needs an explicit
  relayout, since children don't auto-resize with their parent) and
  `SetOnClosing` (`WM_CLOSE`, fired *before* the default handling
  destroys the window — this is the safety-critical hook: the owner
  must release every member here or they're destroyed with the chrome).
  New `ContentRectInClientCoords()` alongside the existing
  `ContentRectInScreenCoords()` (still used by `GrowContentAreaTo`,
  which only needs it for its own size comparisons).
- **`main.cpp`** — `ActivateGroupTab` now uses `SetForegroundWindow`
  on the chrome plus `SetFocus` on the member, not
  `SetForegroundWindow` on the member directly (which doesn't apply to
  a child the way it does a top-level window). `SyncGroupFromForeground`
  is gone — its entire premise (a member independently firing
  `EVENT_SYSTEM_FOREGROUND`) can no longer happen once members are
  children; that event is inherently a top-level-window concept.
  `OnMemberTitleChanged`/the `EVENT_OBJECT_NAMECHANGE` live-title-sync
  hook needed no changes — a window's text property fires that event
  regardless of parent/child status. New `CloseGroup(id)`: releases
  every member, then defers actually erasing the chrome from
  `g_groupChromeWindows` via a posted `kCloseGroupMessage` rather than
  doing it synchronously — erasing it inline would delete the
  `GroupChromeWindow` object (running its own `DestroyWindow`-calling
  destructor) while still unwinding that very object's own `WM_CLOSE`
  call stack. The app-exit path (`WM_DESTROY` on the message window)
  needed the same release-before-destroy treatment added explicitly,
  since destroying that window directly never sends any chrome a
  `WM_CLOSE` at all.
- **Picker window enlarged** (420×480 → 640×620 logical px) — a
  separate, smaller bug reported at the same time: window titles were
  being cut off.

**Verified** with a new compiled harness (same spike methodology as
M0/M4/M5) plus one carefully-scoped live run against the real app: 9
harness checks (frame-stripping, child status, Tab-mode show/hide,
tab-switch visibility flip, Tile-mode simultaneous slots, `ReleaseGroup`
restoring top-level status, **the critical safety case — members
survive chrome destruction after release**, `onResized_`/`onClosing_`
firing correctly) all pass, plus the asynchronous-destruction finding
above (confirmed by deliberately *not* releasing a member and watching
it get destroyed ~1s after its parent). The live run created a real
group from two Notepad windows (matched by an exact title string this
script itself set, never a broad "select all" — the previous test
mistake that swept up real desktop windows is not being repeated),
confirmed both correctly reparented with only one visible, then closed
the group via its own close button and confirmed both windows survived,
returned to independent top-level state, and resumed normal operation
(restore-position sync re-engaged on them immediately, per the log).

### Follow-up usability pass (2026-09-02): flashing bug, configurable hotkey, DPI review

More real-use feedback right after the reparenting rework landed, all
addressed in the same sitting:

- **Real bug, fixed: a tile-mode group with two Notepads visibly
  flashed.** Root cause: `ReflowGroupTo`'s auto-grow-to-fit step
  (`GrowContentAreaTo`) resizes the chrome, which synchronously fires
  `WM_SIZE` -- re-entering `ReflowGroupTo` itself via `onResized_`
  *before* the outer call's own follow-up `ApplyLayout` had run. If the
  grown size didn't converge in exactly one step (plausible with two
  members' minimums side by side in a tile grid), each reentrant call
  ran its own layout pass and could grow again, cascading into repeated
  resize/show/hide cycles -- the flashing. Fixed with a reentrancy
  guard (`g_reflowGrowInProgress`): a nested call during the grow step
  is now a no-op, since the outer call always finishes the job itself
  right after `GrowContentAreaTo` returns. Verified the mechanism is a
  real, confirmed risk via code tracing; could not force this specific
  test environment's default chrome size to actually need growth against
  two Notepads (unlike earlier in this session, on a different display),
  so this fix is code-reviewed and mechanism-confirmed rather than
  visually re-observed fixed -- flagged honestly, not claimed as
  independently re-verified.
- **New: configurable "New Group" hotkey.** Win+Alt+G is claimed by
  something else on this dev machine (a real, reproducible constraint,
  not hypothetical), so a fixed hotkey wasn't viable. Added
  `Settings::groupHotkeyModifiers`/`groupHotkeyVirtualKey` (registry-
  persisted, default Win+Alt+G), a new `src/hook/GroupHotkeyDialog.h/.cpp`
  (four modifier checkboxes + a one-character field -- not a live
  "press your shortcut" capture, which is unreliable for the Win key
  specifically since the shell often intercepts it first), and a new
  tray item "Change Group Hotkey..." wired through `ChangeGroupHotkey`
  in `main.cpp`, which loops the dialog with an inline error if the
  chosen combination is already claimed (checked by actually attempting
  `RegisterHotKey`, not just guessed) rather than silently leaving no
  hotkey registered.
  - **Real bug found and fixed along the way**: the key field's
    `EM_SETLIMITTEXT` was set to 1 to match "exactly one character," but
    since the field starts *pre-filled* with the current key (already 1
    character), there was never room to insert a replacement -- any
    typed key produced `EN_MAXTEXT` and was silently rejected. A user
    would have had to know to clear the field first, which nothing in
    the UI suggested. Confirmed directly (not guessed) via a diagnostic
    log showing the exact `EN_MAXTEXT` notification code. Fixed the
    standard way for a single-key-capture field: allow a little more
    room (4 chars) and auto-trim to just the most recently typed
    character on `EN_CHANGE`, giving the same "typing replaces what was
    there" feel without requiring the user to select/clear first.
    Verified correct via code review of the real keyboard-input path
    (`TranslateMessage`/`DispatchMessageW`, the standard pattern) --
    automated cross-process `WM_CHAR`/`SendInput` simulation of this one
    interaction proved unreliable as a *test* technique in this
    environment (inconsistent results attributable to cross-process
    input-delivery timing, not the fix itself), so this was not directly
    re-observed working end-to-end the way the rest of this session's
    fixes were; flagged rather than glossed over.
- **DPI/multi-monitor code review** (live testing deferred again --
  no second monitor available this session): found and fixed one real
  gap -- `GroupChromeWindow` never handled `WM_DPICHANGED`. Every
  layout/paint calculation already calls `GetDpiForWindow` fresh rather
  than caching a stale value, so the chrome would eventually
  self-correct on the next *unrelated* interaction (a tab click, an
  edit) -- but dragging a group to a different-DPI monitor didn't
  proactively resize/relayout it, so it would sit visibly wrong until
  something else happened to trigger a reflow. Fixed with the standard
  MSDN-documented pattern: resize to the suggested rect Windows provides
  in `WM_DPICHANGED`'s `lParam`, which triggers `WM_SIZE` (and therefore
  the existing `onResized_` relayout) on its own if the size actually
  changed -- no new relayout path needed. Also noted, not actionable:
  reparenting incidentally makes "mixed DPI across a single group's
  members" structurally impossible now (they're all children of one
  parent, necessarily on the same monitor as it), simplifying what used
  to be a real concern under the old reposition-only design. Still
  genuinely untested against real multi-monitor/DPI-switching hardware
  -- this remains open, carried in M6.

### Persistent flashing bug, second attempt (2026-09-02)

The reentrancy-guard fix above did not resolve it -- user re-tested with
2 real Notepads in a Tile group and still saw flashing (screenshot: one
tile rendering blank/white). Root cause reconsidered from scratch:
**this app's *pre-existing* restore-position-sync feature** (tracks
whichever window was last foreground as `g_trackedWindow`, and calls
`SetWindowPlacement` on it whenever `EVENT_OBJECT_LOCATIONCHANGE`
settles) **was never taught that a window can stop being independently
trackable by becoming a group member.** If a Notepad happened to be
`g_trackedWindow` at the moment it was added to a group, nothing clears
that -- reparenting doesn't fire `EVENT_SYSTEM_FOREGROUND`, so
`OnForegroundChanged` (the only place that normally updates/clears
`g_trackedWindow`) never runs for this transition. Every position change
`GroupManager` then makes to that member (parent-client-relative
coordinates) keeps re-triggering restore-sync's own settle-and-
`SetWindowPlacement` logic on the same window (screen/workspace
coordinate semantics) -- two systems fighting over one window's
position. Fixed in `ReflowGroupTo`: after every `ApplyLayout` call,
if `g_trackedWindow` is found to be a member of the group just laid
out, clear it and its settle-tracking state (`g_inMoveSizeLoop`,
`g_pendingSettleRect`, the settle timer) -- the same reset
`EVENT_OBJECT_DESTROY`'s handler already does, just triggered from a
different place since that event never fires for this transition.
**Not independently re-verified live** (session constraints) --
logically sound and traced to a real, specific mechanism via code
reading, not guessed, but flagged honestly rather than claimed fixed
without evidence. Re-test before considering this closed.

### Major scope expansion requested (2026-09-02) -- not yet started

User feedback after using the reparented group feature, verbatim intent
preserved (numbering theirs):

1. **Replace the picker with a persistent two-list management dialog.**
   No Tab/Tile choice in this dialog at all (moved to the toolbar, see
   #2) -- just two lists: "Active windows" (candidates) and "Group"
   (current members). Adding moves a window from Active -> Group;
   removing moves it back Group -> Active (not a checkbox model
   anymore). The Group list is reorderable (drag, presumably, matching
   the existing tab-drag-reorder gesture) -- order matters directly for
   tile/tab layout order (#3, #5). **Same dialog accessible from the
   group's own toolbar**, not just at creation -- i.e. this dialog *is*
   both "New Group" and "Edit windows..." now, unified.
2. **A real toolbar on the group chrome**, replacing today's plain tab-
   strip-or-label header, with controls for:
   - **Alignment**: Horizontal (default) / Vertical -- affects both tab
     placement (#4) and tile grid growth direction (#5).
   - **Tab (default) / Tile** mode toggle (currently a context-menu
     item -- moves to the toolbar).
   - **Window management** -- opens the dialog from #1 (add/remove/
     reorder).
3. **Tab order follows Group-list order** (from #1's dialog) --
   reordering tabs by dragging (already built) must also reorder the
   underlying list, and vice versa; today's `GroupState::Reorder` +
   drag-tab wiring already does the membership-order part, just needs
   to be the *same* order the management dialog's list shows/edits.
4. **Orientation affects tab placement**: Vertical alignment puts tabs
   on the left side (currently always a horizontal strip across the
   top); Horizontal keeps tabs on top. This is a real rendering change
   to `GroupChromeWindow`'s tab strip (currently hardcoded to a
   horizontal strip at the top, `kTabStripHeight`-only layout).
5. **Tile grid growth direction follows alignment**: a grid that grows
   horizontally or vertically depending on the Horizontal/Vertical
   setting, filled from the upper-left in Group-list order. Today's
   `GroupManager::ApplyTileLayout` always computes a roughly-square
   `cols = ceil(sqrt(n))` grid regardless of orientation -- needs an
   orientation-aware column/row-count strategy instead (e.g. Horizontal
   = prefer more columns/wider grid, Vertical = prefer more rows/taller
   grid).
6. **Rename the product-facing feature to "Polish Groups"** and make it
   pinnable to the Start menu. The chrome window's title is currently
   just "Group" (`GroupChromeWindow::Show`'s hardcoded
   `CreateWindowExW` title) -- likely needs an actual per-group name
   (not just the literal string "Polish Groups" repeated for every
   instance) plus whatever's needed for a taskbar-pinnable/Start-
   pinnable identity (AppUserModelID, a real icon instead of relying on
   defaults, etc. -- not yet researched).
7. **Nested groups**: a group can contain other groups as members. The
   entire `GroupMemberKind::NestedGroup` enum case and `nestedGroup`
   field already exist in `GroupState`/`GroupMember` specifically
   because this was anticipated from the very first planning pass (see
   the plan file referenced at the top of this section) -- v1
   deliberately never populated it. This is the milestone that finally
   needs it: real recursive layout (a nested group's chrome would
   itself need to become a reparented child of the outer group, or some
   other containment strategy -- not yet designed), recursive
   reparenting/z-order/show-hide, and recursive `ReleaseGroup` cleanup
   (already-tricky child-destruction-order safety, now one level deeper).
8. **Alt+backtick MRU switching scoped to one group's members**, active only
   when a group chrome is the current foreground window -- cycles
   between that group's own members in most-recently-used order (the
   same MRU-list pattern `ActivationHistory` already implements
   app-wide for real Alt+Tab, but scoped to just one group's
   membership). Needs its own keyboard hook or `WM_HOTKEY`-per-group
   registration strategy -- not yet designed; Alt+backtick specifically may
   also need checking against existing OS/app reservations the way
   Win+Alt+G turned out to be already claimed.

**Scope note**: this is a substantial redesign, not an incremental fix
-- it touches the picker/management-dialog UI, the toolbar and tab-strip
rendering, the tile layout algorithm, the product's naming/identity, and
adds a genuinely new capability (nesting) the architecture was only
ever *shaped* for, never built. Recommend treating this as its own
planning pass (milestones, in roughly the order above since #1-3 are
foundational to #4-5, and #7-8 are more independent/deferrable) rather
than an ad-hoc continuation, given how much has already landed in this
session.

**Milestone plan written (2026-09-02)**, saved at
`C:\Users\david\.claude\plans\i-want-to-make-compressed-dusk.md`: M1
(data model: `GroupAlignment` + per-group `Name()`) through M9 (Alt+`
MRU switching). Two decisions locked in during planning: group names
are user-editable (not auto-generated), and the alignment/mode/manage-
windows controls live in the group chrome's own custom-drawn title bar
as three small icon toggle buttons -- not a separate toolbar row --
so no content space is taken from member windows. M4 (the custom title
bar, replacing the native one via `WM_NCCALCSIZE`/`WM_NCHITTEST`) is
flagged as the riskiest single piece and starts with a throwaway spike
before touching `GroupChromeWindow` for real.

**M1 done.** `GroupState` gained `GroupAlignment` (`Horizontal`
default) and a `std::wstring name_` (auto-defaulted to `L"Group
<id>"` at construction), with `Alignment()`/`SetAlignment()` and
`Name()`/`SetName()` mirroring the existing `Mode()`/`SetMode()` pair.
3 new tests in `GroupStateTests.cpp` (default alignment/name,
`SetAlignment` leaves membership untouched, `SetName` overrides the
auto-generated default) -- 39/39 tests pass. Pure plumbing, no UI
wiring yet (that starts at M2).

**M2 + M3 done together** (landed as one pass -- M2's two-list dialog
literally returns a final membership order, and the old add/remove
diff logic couldn't express reordering at all, so shipping M2 without
M3 would have left the dialog's drag-to-reorder doing nothing real).

- `GroupState` gained `SetMembers(const std::vector<HWND>&)`: replaces
  membership wholesale in one call (drop/add/reorder), re-deriving the
  active member by identity the same way `Reorder` already does,
  falling back to the first member if the previously-active one was
  dropped. 4 new tests -- 43/43 tests pass.
- `GroupPickerWindow` rewritten: two side-by-side `WC_LISTVIEWW` lists
  ("Active windows" / "Group", no more checkboxes), Add/Remove buttons
  plus double-click (`LVN_ITEMACTIVATE`) to move a row between them,
  and drag-to-reorder within the Group list only (`LVN_BEGINDRAG` +
  live reorder-on-crossing during `WM_MOUSEMOVE`, same pattern as
  `GroupChromeWindow`'s own tab drag-reorder -- not a separate
  insert-mark line). A name field (`EDIT` control) replaces the old
  Tab/Tile radio buttons entirely -- mode selection is gone from this
  dialog, moving to M4's title-bar controls; every new group starts in
  Tab mode until M4 lands (unchanged from today's actual default, just
  no longer chosen here).
- `GroupPickerResult` is now `{windows, name}` (no `mode` field).
  `main.cpp`'s `TriggerNewGroup`/`EditGroupWindows` updated: creation
  calls `CreateGroup(windows)` (mode defaults `Tab`) then
  `SetName(selection->name)`; editing calls `SetMembers`+`SetName`
  instead of the old manual add/remove/mode diff. Chrome title
  (`SetWindowTextW`) now shows the group's real name instead of the
  hardcoded `"Group"` string, on both creation and edit.
- Build clean, 43/43 tests pass, smoke-tested (clean startup, no log
  errors). **Not yet live-verified**: actually opening the dialog and
  exercising Add/Remove/double-click/drag-reorder/name-edit by hand --
  flagging this explicitly since list-view drag-and-drop specifically
  was called out in planning as the riskiest part of M2 to get right,
  and time didn't allow a live pass this round. Please try creating
  and editing a group before M4 builds on top of this.

### Two more real bugs, fixed (2026-09-02, same day)

User confirmed the restore-sync-conflict flashing fix worked. Two more
issues from continued real use:

- **Blank tab until mouse-over, fixed.** Switching Tab-mode tabs left
  the newly-shown member visually blank until the user moved the mouse
  over it. `SWP_SHOWWINDOW` makes a window visible but doesn't guarantee
  it actually repaints -- a window that was just hidden (or freshly
  reparented) can sit on a stale/uncomposited DWM redirection surface.
  Fixed in `GroupManager`'s `PositionMember`: an explicit
  `RedrawWindow(..., RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN |
  RDW_ERASE)` right after showing a member (`RDW_ALLCHILDREN` matters
  for apps like Explorer that are themselves composed of child panes).
- **New: tab hover thumbnail preview.** Hovering a tab for ~400ms now
  shows a small live DWM thumbnail of that member, so you can see which
  window it is before switching. New `src/hook/GroupTabThumbnail.h/.cpp`
  -- a plain (non-layered) `WS_POPUP` hosting a `DwmRegisterThumbnail`
  preview, reusing the exact technique already confirmed working during
  the original Alt+Tab research (M0(b) of that earlier plan: DWM
  thumbnails work fine on a plain popup, no `WS_EX_LAYERED` needed).
  `GroupChromeWindow` gained `WM_MOUSEMOVE`/`WM_MOUSELEAVE`/`WM_TIMER`-
  based hover detection (`TrackMouseEvent` re-armed each move, a 400ms
  debounce timer) and a new `SetOnTabHovered` callback reporting the
  hovered index and its screen rect; `main.cpp`'s `OnGroupTabHovered`
  owns one shared, lazily-created `GroupTabThumbnail` instance and looks
  up the real member `HWND` via `GroupState`.

Both build clean, all 36 tests pass, app starts and runs without error
in a quick smoke test. Neither was re-verified live end-to-end this
session (time constraints on both sides) -- please confirm when you get
a chance.

## History (condensed)

Polish went through three abandoned UI directions before landing on the
current, UI-free fix — each is described in more detail in git history
if the reasoning ever matters again, but the short version:

1. **A custom always-visible overlay button** drawn next to the minimize
   button. Abandoned: clicking it triggered a spurious foreground-change
   event that broke tracking, and it was the buggiest, most
   Win32-subtlety-laden part of the app.
2. **Shift+click a window's real maximize button** to toggle between its
   last two rects, with a hover-tooltip hint. The toggle itself worked,
   but the hint could never be made reliable — Windows 11's own Snap
   Layouts flyout appears on hover over any maximize button, independent
   of any modifier key, and covers anything drawn in that same spot.
3. **Shift+Right-click anywhere on the title bar → radial (pie) menu**
   (Toggle/Maximize/Minimize/Snap Left/Snap Right), specifically to dodge
   the flyout-collision problem above. Fully implemented and
   build-verified, but abandoned mid-testing when the user reframed the
   actual problem: the annoyance wasn't "no quick way to toggle," it was
   "Windows' own restore position is wrong after a snap" — fixable
   directly, with no new gesture needed at all. `src/hook/RadialMenu.*`
   and `src/hook/TitleBarMenuHook.*` were deleted once that was
   confirmed as the right direction.
4. **Toggle itself (`RectHistory`, `HotkeyManager`, Win+Alt+T)** was kept
   briefly as a secondary keyboard-only action after the radial menu was
   deleted, but once the user decided they didn't need the hotkey either,
   it had no trigger left at all — deleted along with it rather than kept
   as unreachable code.

**Win32 gotchas hit along the way, worth remembering for future work in
this codebase:**

- `SetWindowPos`'s `hWndInsertAfter` places a window *behind* the given
  handle, not in front of it.
- `Gdiplus::Graphics(HDC)` does not reliably preserve alpha into a
  backing DIB; a `Gdiplus::Bitmap` needs to wrap the DIB's own memory
  directly (`PixelFormat32bppARGB`, external buffer via
  `CreateDIBSection`), with alpha manually premultiplied before
  `UpdateLayeredWindow`.
- `app.manifest`'s `dpiAwareness` element must use the
  `http://schemas.microsoft.com/SMI/2016/WindowsSettings` namespace, not
  2017 (that's for `longPathAware`) — using 2017 fails silently, the app
  still launches but falls back to DPI-unaware.
- `TTM_ADDTOOL` (and comctl32 calls generally) silently fail unless
  `app.manifest` declares a Common Controls v6 dependency
  (`Microsoft.Windows.Common-Controls`, version `6.0.0.0`) — without it,
  comctl32 loads against an ABI whose struct sizes don't match what
  current SDK headers expect.
- Don't convert between `GetWindowRect()` and DWM extended-frame-bounds
  when recording/restoring a rect — the invisible resize-border inset
  isn't a fixed per-window constant (it differs for edges flush against a
  Snap boundary vs. free-floating), so a round-trip conversion introduces
  a few pixels of position error. Record and restore raw `GetWindowRect()`
  values directly; no conversion needed.
- PowerShell tool invocations used for manual testing are DPI-unaware by
  default and each is a fresh process — call
  `SetThreadDpiAwarenessContext(-4)` at the top of every script that
  touches window rects, or coordinates get silently virtualized.
- This dev machine has something claiming a broad range of
  `Win+Alt+<letter>` hotkeys — confirmed again, directly, when the new
  group feature's own `Win+Alt+G` hotkey failed to register
  (`RegisterHotKey` → `ERROR_HOTKEY_ALREADY_REGISTERED`). Whatever it is
  hasn't been identified; needs tracking down (or testing on a clean
  machine) before relying on any `Win+Alt+<letter>` hotkey here again.
- A plain `std::wofstream` with no explicit codecvt facet cannot
  represent non-ASCII characters and silently truncates the rest of the
  line (including the trailing newline) the moment it hits one,
  corrupting the log stream for everything written after — convert to
  UTF-8 (`WideCharToMultiByte`) and write via a narrow `std::ofstream`
  instead.
- A non-ASCII character inside a wide string literal, in a source file
  saved without a BOM, is silently mis-decoded by MSVC at compile time
  (using the system codepage) unless `/utf-8` is passed as a compile
  option — no warning, no error, just corrupted text baked into the
  binary.

### Tile mode: no header, resizable splitters (2026-09-03)

Two requests, both shipped and verified via a compiled spike (real
Notepad windows, screenshotted before/after a simulated drag — see
`spike_tile_visual.cpp` in scratchpad):

1. **Removed Tile mode's custom header entirely.** It only ever showed
   a plain "Group (N window(s), tiled)" label — pure duplication, since
   the native OS title bar already shows the group's real `Name()`.
   `GroupChromeWindow::HeaderHeight` now returns 0 for Tile mode (was
   always `kTabStripHeight`), handing that space back to the tiles.
2. **User-resizable tile splitters.** `GroupState` gained
   `tileColumnFractions_`/`tileRowFractions_` (each a vector of
   fractions summing to 1.0, empty until customized). `GroupManager`:
   `ApplyLayout`/`ApplyTileLayout` now take `GroupState&` (were
   `const&`) since they auto-populate equal-split fractions the first
   time a shape is seen and whenever the grid reshapes (a member
   added/removed changes the column/row count, invalidating old
   fractions); caches the resulting pixel boundaries
   (`TileColumnBoundaries`/`TileRowBoundaries`) for the chrome to
   render; `SetTileBoundary` applies a drag, touching only the two
   adjacent cells' fractions. `GroupChromeWindow` renders a thin
   draggable bar at each boundary, hit-tests it (with slop) on
   `WM_LBUTTONDOWN`, shows `IDC_SIZEWE`/`IDC_SIZENS` on hover
   (`WM_SETCURSOR`), and clamps every drag so neither adjacent tile
   shrinks below `kMinTileSize` (80 logical px) — enough to still see
   that a window is there, not shrunk to nothing. Fires
   `onTileSplitterDragged_` live (once per mouse-move, not just on
   drop), which `main.cpp`'s `OnTileSplitterDragged` turns into
   `GroupManager::SetTileBoundary` + `ReflowGroupTo`.

Deliberately out of scope for this pass: the earlier-abandoned M6
milestone (alignment-biased wide/tall grid shape) — the grid's
column/row *count* is still the same `ceil(sqrt(n))`-based square-ish
formula as before; only per-cell *sizing* is now user-adjustable.

**Follow-up, same day**: three real problems in the first version above,
all fixed and verified via the same spike (before/during-drag/
after-release screenshots):

- **Splitters were drawn *over* the members' shared edge**, not given
  their own reserved space — too thin to reliably grab, and the
  overlap was the likely cause of visible update artifacts after a
  drag. Fixed properly, not papered over: `GroupManager::ApplyLayout`/
  `ApplyTileLayout`/`SetTileBoundary` all gained a `splitterWidthPx`
  parameter that reserves real gap space between adjacent columns/rows
  in the grid math itself (content-only fractions exclude the gaps
  entirely; cached boundaries are gap *centers* in real coordinates) —
  members now never overlap a splitter. Widened `kSplitterWidth`
  8px→16px-effective (`Scale`d) in the process, and
  `GroupChromeWindow::TileSplitterWidthPx()` is the single source of
  truth both `main.cpp` and `GroupManager` read it from, so the two
  sides can't drift out of sync.
- **No hover feedback.** Added `hoveredSplitter_` tracking
  (`WM_MOUSEMOVE`/`WM_MOUSELEAVE`, narrow per-splitter invalidate via
  the new `InvalidateSplitterBand`) and render the hovered (or
  actively dragged) splitter in `GetAccentColor()` (new in
  `util/DarkMode.h/.cpp`, reads `HKCU\...\DWM\AccentColor` — same
  "no public API, read the registry" pattern as dark-mode detection)
  instead of the plain border color — the same visual language Windows
  itself uses for "this is draggable."
- **Visible update artifacts after releasing a drag** — confirmed via
  spike screenshot (a stray black rectangle in the status bar,
  present right after the drag, gone one frame later). Fixed via one
  final `RedrawWindow(RDW_ALLCHILDREN | RDW_UPDATENOW | ...)` on the
  chrome itself when `WM_LBUTTONUP` ends a splitter drag — cheap
  (once per drag, not once per mouse-move) and guarantees a fully
  clean frame regardless of any transient repaint race during the
  drag itself. Also narrowed `SetTileSplitters`' own invalidate from
  the whole window to just a band around each old+new boundary
  position, matching the same "don't invalidate more than you need to"
  lesson already learned for the tab strip's own hover highlight.

### Tile splitter polish: double-click-to-toggle, thinner resting look (2026-09-03)

- **Double-click a splitter to snap to 50/50**, double-click again to
  restore whatever custom split it had before. `GroupChromeWindow`
  gained `CS_DBLCLKS` (off by default, required for
  `WM_LBUTTONDBLCLK` to ever fire) and a `WM_LBUTTONDBLCLK` case that
  hit-tests the splitter and fires a new
  `onTileSplitterDoubleClicked_` callback. `main.cpp` owns the actual
  toggle logic: `g_tileSplitterLastCustom`, keyed by
  `(GroupId, column, index)`, remembers a pair's fractions right
  before snapping to even, and clears once restored.
- **Resting-state splitters now match the Windows 11 convention**
  (Windows Terminal/VS Code/Settings-app style): a thin
  `kSplitterRestWidth` (2px logical) hairline at rest, growing to the
  full grabbable `kSplitterWidth` and switching to the accent color on
  hover/drag — the reserved gap and hit-test target don't change size,
  only what's painted at rest.

### Real bug, fixed: reparented-member flashing (2026-09-03)

User reported a group with even a single Notepad member flashing
nonstop, and pushed back hard (correctly) after two guessed fixes in a
row didn't hold up — a chrome-window-tracking exclusion for
restore-sync, and a `g_trackedWindow`-clear reordering. Both were real,
defensible issues (kept, since neither is wrong to have fixed) but
neither was *the* cause. What actually found it: adding direct,
per-call-site diagnostic logging (`LogDebug` calls tagging every
`ReflowGroupTo` entry, every `SetWindowPos` in `PositionMember`, every
chrome `WM_PAINT`/`WM_SIZE`, and — the one that mattered — every
`InvalidateRect(window_, nullptr, ...)` call site individually) and
reading back a live repro's log, rather than theorizing further.

**Root cause**, confirmed directly in the log: a reparented member
(a modern Notepad instance) fires `EVENT_OBJECT_NAMECHANGE` for its
own title (`idObject`/`idChild` already correctly filtered to
`OBJID_WINDOW`/`CHILDID_SELF`, so this isn't some noisier child
control) continuously — many times per second — even though the title
text itself never changes. Each event drove
`OnMemberTitleChanged` → `SetMemberTitles`/`SetMemberIcons`, and both
did an unconditional full-window `InvalidateRect`. That unconditional
repaint, firing every few milliseconds, *was* the flashing. Root OS
cause of the repeated `NAMECHANGE` itself is still unconfirmed (a
reparented-into-another-process's-tree top-level window seems to
provoke it) — not chased further since it didn't need to be.

**Fix**: `SetMemberTitles`/`SetMemberIcons`
(`GroupChromeWindow.cpp`) now compare against the currently-cached
value first and skip the repaint entirely when nothing actually
changed. Correct regardless of why the upstream event fires. User
confirmed live: no longer flashes.

**Lesson, worth keeping**: when a real, reproducible bug survives a
plausible first fix, add targeted logging and get a live repro's log
*before* proposing a second fix — don't chain guesses. The eventual
fix here took two rounds of instrumentation (the first round proved
the earlier fixes weren't it and pointed at full-window invalidates;
the second round, tagging each invalidate call site individually,
named the exact caller) and found the real cause in minutes once the
log existed, after two guesses that didn't.

### Future ideas / backlog (not started, just captured)

Requested in passing, worth remembering but not yet designed or
scoped:

- **Clipboard copy flash**: a brief visual flash at the point something
  is copied to the clipboard, so it's obvious what was just copied and
  from where.
- **Paste history popup**: on paste, a popup letting you cycle through
  clipboard history rather than only ever pasting the most recent item.
- **Quick Access rename without renaming the underlying file/folder**:
  today Windows' Quick Access only shows the real name; want a
  friendly alias independent of the actual filesystem name. Should
  support files, not just folders (Quick Access is folder-only today).
- **Radial start menu**: opens from the center of the screen, 8-way
  drag to pick an item; a submenu can open its own 8 options, with the
  opposite drag direction closing back out of it (mirroring how you
  opened it).
- **Thinner, solid Alt+Tab highlight border**: switch the Alt+Tab
  selected-window highlight (`AltTabHighlightBorder`) to a thinner
  line, about the same size as the tile-mode resize splitter, and
  fully opaque (no transparency) instead of its current look.
- **Taskbar icon click cycles windows**: clicking a taskbar icon that
  has 2+ windows grouped under it should cycle through them (matching
  a behavior some other taskbar tools/older Windows versions have),
  rather than just showing the thumbnail preview strip or activating
  whichever was last active.
- **Reorder windows within a taskbar icon's group**: let the user
  reorder the windows grouped under one taskbar icon (affects the
  order they're cycled through and/or shown in the thumbnail strip).
- **Alt+Tab as a full list, not just cycling**: show every open window
  as a list so you can see how many there are and where you currently
  are in the cycle, not just one highlighted window at a time. Support
  clicking directly on a window in the list to jump straight to it,
  and arrow-key navigation through the list (not just repeated Tab).
- **Alt+Tab minimized-windows list**: show minimized windows in their
  own list, reachable via click or cursor keys, but *not* mixed into
  the main Alt+Tab cycle order itself (today `IsCandidateWindow`-based
  cycling already excludes minimized windows from the cycle -- this
  would add a separate, deliberately-reached view for them instead of
  surfacing them unprompted).
- **Consider switching to WinUI3** for a more modern/up-to-date GUI
  look, instead of the current plain GDI-drawn chrome. Open question,
  not a decision -- would touch essentially every custom-drawn window
  in this codebase (`GroupChromeWindow`, `GroupPickerWindow`,
  `GroupTabThumbnail`, `AltTabHighlightBorder`/`AltTabDimOverlay`,
  `GroupHotkeyDialog`) and is a large enough shift (different
  packaging/deployment model, XAML Islands or a full WinUI3 app host)
  to warrant its own research/spike pass before committing, not
  something to fold into an unrelated fix.
- **Improve Windows virtual desktops**: persist desktop layout to a file
  and reload it on login (today's virtual desktops don't survive a
  restart/sign-out); pin one app to show on all desktops
  simultaneously (Windows supports this per-window today via right-click
  on the taskbar icon -> "Show this window on all desktops", but the
  idea here is a more discoverable/managed way to do it); and mark
  which desktop you're currently on somewhere always visible (today
  there's no persistent on-screen indicator, only the transient
  Task View overlay). Not yet researched -- virtual desktop state isn't
  exposed via a public documented Win32 API (the relevant `IVirtualDesktop*`
  COM interfaces are undocumented/private, reverse-engineered by
  third-party tools like VirtualDesktop11), so a spike into what's
  actually readable/controllable would need to come before any real
  design.

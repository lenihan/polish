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
  `Win+Alt+<letter>` hotkeys — moot now that the hotkey is gone, but
  relevant again if a hotkey is ever reintroduced.

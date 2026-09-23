# Known Limitations

These are permanent, by-design constraints of how Windows works, or
deliberate scope cuts for the current build — not bugs to fix. This doc
gets a full pass in Phase 3; today it records what's already known.

1. **Elevated (Run as Administrator) windows are inaccessible** to an
   unelevated instance of Polish, due to Windows' UIPI (User Interface
   Privilege Isolation) — restore-position sync is blocked for these, and
   in Alt+Tab an elevated window can be dimmed/undimmed correctly but its
   turn at being visually promoted to the front can silently fail (logged
   as a warning) if something else happens to be covering it. This is the
   one case with no unelevated workaround. (A tray "Restart as
   Administrator" option is planned for Phase 3 but not yet built.)

2. **Multi-monitor / mixed-DPI setups** are supported via Per-Monitor-V2
   DPI awareness, which depends on `app.manifest` being embedded correctly
   in the build.

3. **A window is only tracked for restore-position sync once it's been
   the foreground window at least once since Polish started.**
   `EVENT_SYSTEM_FOREGROUND` is what starts tracking, so a window sitting
   untouched in the background since before Polish launched isn't synced
   until the user switches to it — but once they do, sync applies
   immediately using whatever rect it's *currently* sitting at, with no
   need to move/snap it again. (Alt+Tab does not share this limitation —
   its candidate list is built from every currently open window via
   `EnumWindows`, not just ones already focused since Polish started.)
   Full always-on tracking of every window regardless of foreground state
   is Phase 2 scope.

4. **Restore-position sync only ever writes `rcNormalPosition` — it never
   moves or resizes a window on its own.** It piggybacks entirely on rects
   Windows itself already produced (a snap, a drag, a resize); Polish
   never initiates a placement change as part of this feature. If a
   window's position is changed by something Polish never observes (e.g.
   while unelevated Polish can't see an elevated window, per #1), its
   restore position can't be corrected either.

5. **Alt+Tab's candidate list is a snapshot taken once when a session
   starts**, not live-updated while Alt is held — a window that closes or
   gets minimized mid-session isn't removed from the list until the
   session ends.

6. **The "Start with Windows" checkbox only checks whether the Run
   registry key's value exists, not Windows' separate `StartupApproved`
   state.** Task Manager's own Startup Apps tab can disable an entry
   without removing it from the Run key — if that happens, the checkbox
   still shows checked even though Windows won't actually run Polish at
   login.

7. **A UWP/Store app window (Calculator, Settings, Photos, ...) can never
   join a group** — confirmed live, `SetParent` fails outright with
   `ERROR_INVALID_PARAMETER` for the `ApplicationFrameWindow` class every
   time, mixed-DPI hosting opt-in or not. Rather than attempt it and fail
   silently, such a window is refused up front: the group picker lists it
   greyed out (`WindowFilters::IsUnreparentableWindow`), with a tooltip
   explaining why, and its Add button/click/double-click/keyboard-add are
   all disabled. `GroupManager::ApplyLayout` also drops one defensively if
   it ever ends up a member some other way (e.g. a window that changes
   class after joining), the same path used for any member that turns out
   to be unreparentable.

   An earlier version of Polish instead let such a window join as a
   top-level "attached" member, hand-driving its position, Z-order,
   minimize/restore and candidate filtering. That approach was abandoned:
   its taskbar button could not be hidden by any mechanism tried
   (`WS_EX_TOOLWINDOW`, `ITaskbarList::DeleteTab`,
   `IApplicationView::SetShowInSwitchers` all reported success and did
   nothing — an OS-level restriction, not a Polish bug), and there was an
   unresolved sizing glitch switching Tile/Stack alignment with such a
   member.

8. **The active-window halo (`ActiveWindowHalo`) sits in the normal
   (`HWND_TOP`) Z-order band, not `WS_EX_TOPMOST`, so any always-on-top
   window still covers it wherever they overlap.** Confirmed live: for a
   window sized to the work area (i.e. right up against the taskbar,
   the extremely common near-maximized case), the bottom edge's glow band
   is drawn correctly but sits entirely underneath the (always-on-top)
   taskbar, making it invisible in practice on that one edge. This is the
   deliberate tradeoff described in the halo's own class comment — being
   topmost would mean the halo painting over every other app's window
   too, not just the target's own — not something planned to change.

9. **The halo's virtual-desktop-follow fix is not yet built, only logged.**
   A layered top-level window belongs to whichever virtual desktop it was
   created on, so a halo created once at startup could in theory stop
   appearing after switching desktops. `ActiveWindowHalo` already logs
   every virtual-desktop-id change it observes via
   `IVirtualDesktopManager::GetWindowDesktopId` (see
   `FollowTargetVirtualDesktop`), but doesn't yet call
   `MoveWindowToDesktop` — that's deliberately deferred until the log
   confirms this is a real, observable problem on a real multi-desktop
   session, not a hypothetical one.

10. **The halo follows the OS theme, not the desktop wallpaper**, so a
    white halo on a light wallpaper in dark mode (or a black halo on a
    dark wallpaper in light mode) reads faint. `polish::GetAccentColor()`
    is the documented fallback if this turns out to matter in practice —
    not implemented, since it would mean reinstating a real per-pixel
    premultiply (today's renderer exploits pure white/black to premultiply
    with a single store).

11. **Bullseye only flashes for Ctrl+V and Shift+Insert pastes.** Copy is
    detected via the OS clipboard listener, so every real copy counts
    (Ctrl+C, right-click > Copy, a toolbar button). Paste has no such
    signal -- the OS never says "an app just pasted" -- so it's inferred
    from the keystroke, and right-click > Paste, menu Paste and
    drag-and-drop don't flash.

12. **Bullseye's anchor falls back for apps with no Win32 caret.**
    Chromium, Electron and UWP apps draw their own text caret and expose
    none through `GetGUIThreadInfo`, so the ring centers on the mouse
    pointer (if it's over the window) or the window's middle instead. The
    log line for each animation records which one was used.

13. **Bullseye is `WS_EX_TOPMOST`, unlike the halo** (see #8) -- it
    deliberately draws over every window, the taskbar included, for its
    third of a second. It's suppressed during a full-screen game or
    presentation, but not over a maximized window.

14. **Bullseye may not appear on another virtual desktop.** Like the halo
    (#9), its window belongs to the desktop it was created on and isn't
    moved with the foreground window. Not yet confirmed or fixed.

15. **Alt+backtick can't see an elevated app's tabs.** Deciding whether an
    app is one whose tabs can be read means opening its process to find
    its executable, which Windows refuses for an elevated (Run as
    administrator) app from a normal one. Alt+` pressed in such a window
    does nothing. Same root cause as #1.

16. **Alt+backtick is bound to a key, not a character.** It is the
    `VK_OEM_3` key, which is the backtick key on US and UK layouts but a
    letter on German and other layouts (and a dead key on some). On those
    layouts Alt plus the key in that position triggers it instead. The
    log records what character the key types on the active layout at
    startup.

17. **Alt+backtick only works in apps whose tabs can be read.** Tabs are
    not operating-system objects: they exist only inside an app's own UI,
    and the only route to them is UI Automation, which every app answers
    differently. Polish carries a per-app allowlist (see
    `src/tabs/TabSwitching.h`) and does nothing in an app that isn't on
    it. Probed live on this project's machine:

    | App | Tabs readable? |
    | --- | --- |
    | VS Code | yes, cleanly |
    | File Explorer | yes |
    | Windows Terminal | yes |
    | Notepad | yes, but titles read as the accessibility label ("top. Modified.") |
    | Edge | partially -- see #18 |
    | Outlook, OneNote | no -- only *ribbon* tabs (Home/Insert/View) are exposed, never document tabs, so they are deliberately excluded |

    The allowlist fails closed for exactly that last reason: a generic
    "switch between every tab-like thing" sweep would offer ribbon tabs
    and VS Code's own sidebar icons as switch targets.

18. **Edge reports only some of its tabs.** With seven tabs open, Edge
    exposed three to UI Automation; the rest are not present in the tree
    at all, most likely because they are unloaded or unrealized. Alt+`
    therefore shows an incomplete list in Edge. It also reports the tabs
    it does expose twice, through two nested containers, which Polish
    de-duplicates by runtime id.

19. **Tab order is learned by observation, not watched continuously.**
    Most-recently-used ordering is built from what Polish sees each time
    the switcher runs -- which tab was frontmost -- plus its own
    switches. It holds no live accessibility listener on the foreground
    app, which would cost far more than it would buy. So switching tabs
    by hand several times between two Alt+` presses is only observed as
    the last of those switches, and a window's order is unknown until
    the switcher has been used in it once.

20. **The halo waits out a restore animation rather than following it.**
    When a window is restored from the taskbar, Windows animates it
    growing into place -- but that animation is a DWM visual effect and
    is invisible to an outside process. Measured live:
    `EVENT_OBJECT_LOCATIONCHANGE` fires exactly once, at the same moment
    as `EVENT_SYSTEM_MINIMIZEEND`, already carrying the window's final
    rect, with `IsIconic` already false. There are no intermediate rects
    to follow and no event marking the animation's end, so the halo is
    held off for a fixed delay instead (`kHaloRestoreDelayMs`) and then
    shown. A window restored on a machine with minimize/restore animation
    switched off skips the wait entirely.

21. **Finding the text caret takes three different APIs, and no one of
    them works everywhere.** The copy/paste bullseye aims at the middle of
    the selected text, which UI Automation reports reliably. With nothing
    selected -- any paste -- it needs the caret instead, and that is where
    it gets awkward. All three of these were needed, in this order, and
    each was established live:

    - **UIA's selection**, for a real selection. Solid everywhere probed.
    - **MSAA `OBJID_CARET`**, for the caret. Correct in Chromium (Edge's
      address bar tracks properly through it), and the reason a caret can
      be found at all in apps that draw their own and expose no Win32 one.
      Returns `S_FALSE` and all zeroes for a XAML control such as
      Explorer's address bar, which sits behind an `InputSiteWindowClass`
      host with no MSAA caret.
    - **UIA's caret**, a collapsed selection range, last. It needs a
      workaround of its own: a degenerate range has no bounding rectangles
      (documented, and what Explorer returns), so the range is cloned and
      stretched by one character to have something measurable -- forward
      normally, backward when the caret is at the very end of the text,
      taking the near or far edge of that character accordingly.

    This one is last because it is the least trustworthy: Chromium's
    omnibox answers it with a plausible-looking 2px caret rect pinned 8px
    inside the control's left edge, identical wherever the caret actually
    is. MSAA answers correctly there, so in the one app known to lie this
    value is never reached. Below all three, the older caret → mouse →
    window-centre chain still applies.

22. **The Windows 11 taskbar's hover thumbnail flyout cannot be
    suppressed by any *registry or z-order* means, and cannot be read at
    all, from outside `explorer.exe` -- but it CAN be prevented by owning
    the pointer over the buttons.** Five approaches were built and
    measured against build 26200 (25H2). Four failed; the fifth works and
    is what Polish uses. Recorded in full because four of these look like
    they should work, and one of them is the obvious first idea.

    - **Swallowing `WM_MOUSEMOVE` in a `WH_MOUSE_LL` hook while the cursor
      is over the taskbar** -- returning non-zero also **freezes the
      cursor**, since the same input processing the hook gates is what
      moves the pointer. Measured with a trap rect in the middle of the
      screen: the cursor walked to the trap's edge and stopped dead,
      unable to enter. This would pin the pointer against the taskbar, so
      the whole technique is unusable, not merely imperfect.
    - **`ExtendedUIHoverTime` set high** (the widely-repeated registry
      trick) -- no effect at all, with *and* without an `explorer.exe`
      restart. The flyout appeared on schedule both times.
    - **Covering it with a `WS_EX_TOPMOST` panel** -- the flyout draws on
      top, even when the panel re-asserts `HWND_TOPMOST` *after* the
      flyout is already on screen.
    - **Raising a window's z-band with `SetWindowBand`** -- every band
      from 1 to 18 refused with `ERROR_ACCESS_DENIED`. That API is gated
      behind UIAccess, and UIAccess only grants `ZBID_UIACCESS` (band 2)
      while shell UI reaches `ZBID_SYSTEM_TOOLS` (band 16). **So UIAccess
      is not a way around this either** -- worth stating explicitly, since
      it looks like one.

    The flyout is also **completely invisible to UI Automation**: with it
    plainly on screen showing two thumbnails, a full `FindAll` under
    `Shell_TrayWnd` returned the same 23 descendants as when it was
    absent, and zero thumbnail elements. Any detection of it has to be
    visual or positional. No HWND appears or disappears with it either;
    `ThumbnailDeviceHelperWnd` (explorer, band 16, permanently visible) is
    1x1 and is not the renderer.

    **What does work: a "shield" window over the button strip.** A
    layered, topmost, `WS_EX_NOACTIVATE` window placed exactly over the
    taskbar's app buttons, returning `HTCLIENT` from `WM_NCHITTEST`, owns
    the pointer in that strip. The taskbar therefore never receives a
    pointer-enter, its `HoverFlyoutController` never starts its dwell, and
    **no flyout is ever created**. Confirmed by a back-to-back A/B at the
    same hover point: without the shield the flyout appeared with both
    thumbnails; with it, nothing.

    Why this succeeds where the four above fail: it gates nothing in the
    input stream, so unlike swallowing `WM_MOUSEMOVE` the cursor moves
    completely normally; and it never has to out-draw the flyout, so the
    z-band wall is irrelevant -- there is no flyout to out-draw. A plain
    topmost window *can* sit above `Shell_TrayWnd` (measured at z-depth 8
    against the taskbar's 14), which is the same thing the bullseye
    already relies on (#13).

    Constraints that come with it:

    - **Alpha must be 1, not 0.** A fully transparent layered window is
      excluded from hit-testing, so alpha 0 would receive nothing and
      block nothing.
    - **The shield owns every event in that strip while it is closed**, so
      anything the native taskbar would have done there (the right-click
      jumplist, middle-click, drag-and-drop onto a button) has to be
      handed back deliberately. How, and what does *not* work, is the
      next entry. The left button is the exception: Polish claims plain
      click (cycle), Shift+click (cycle backwards) and Ctrl+click (toggle
      to the most recent other window). Native shift+click, which opens a
      new instance, moves to middle-click, which already did the same
      thing. Ctrl still hands everything else back -- hover, right-click
      and middle-click all behave as they would without Polish.
    - **The taskbar's own hover highlight is lost**, since the taskbar no
      longer sees the pointer. Polish has to draw its own or accept its
      absence.
    - The shield has to track the strip as it moves, and **how fast it
      tracks is a correctness property, not a polish one**. Buttons shift
      whenever an app opens or closes -- on a centered taskbar the whole
      row re-centers, so every button moves -- and until the shield is
      re-read it is covering where the buttons *were*. Hovering the part
      it no longer covers gets the native flyout.

      This was found in use, not in testing, and is invisible to any test
      that does not open an app mid-hover. A 3-second safety-net poll was
      the only thing re-reading the strip, so the gap ran to 3 seconds.
      It is now driven by `EVENT_OBJECT_LOCATIONCHANGE` filtered to
      taskbar-owned windows (the only event that fires for a re-layout
      with no window lifecycle behind it), plus foreground and destroy,
      debounced by 120ms -- measured at 210ms from a new window appearing
      to the shield being correct.

      **210ms is under the native flyout's own dwell, which is what makes
      this a fix rather than a narrowing.** That dwell measured between
      250ms and 450ms on this build: hovering for 250ms produces nothing,
      450ms produces the full flyout. A gap shorter than the dwell cannot
      produce a flyout, because the dwell never completes inside it.

    Hooking `HoverFlyoutController::ShowTaskListButtonHoverFlyout` inside
    `explorer.exe` (what Windhawk does, via per-build PDB symbols) is the
    only *other* way, and is no longer needed for this.

    Separately, and unaffected by any of the above: swallowing mouse
    **buttons** in a low-level hook is fine -- only swallowing *moves*
    freezes the cursor.

23. **Handing a single event back to the taskbar, from under the shield,
    takes all three of: `WS_EX_TRANSPARENT`, swallowing the press, and
    replaying it from the message loop.** Each was arrived at by a
    measurement that contradicted the obvious answer, so all three are
    recorded.

    - **`HTTRANSPARENT` from `WM_NCHITTEST` does not work across a
      process boundary.** It is documented to pass the point to
      underlying windows *in the same thread*, and the taskbar is another
      process, so against `Shell_TrayWnd` it absorbs exactly as
      `HTCLIENT` does. This is a convincing wrong answer rather than an
      obviously wrong one: the shield really did return `HTTRANSPARENT`
      (confirmed by sending it `WM_NCHITTEST` directly and reading back
      `-1`), and the flyout really did stay away -- because the pointer
      was still being absorbed, which looks identical to success from the
      outside. It was only caught by testing the Ctrl escape hatch, where
      "no flyout" is the *failure*.
    - **`WS_EX_TRANSPARENT` does work across a process boundary**, which
      is why the shield toggles that style per gesture instead. With it
      set, `WindowFromPoint` over the strip returns `MSTaskSwWClass` and
      the native flyout comes back; with it clear, the shield absorbs.
      Note this reverses nothing about the shield's design -- the style
      must still be *off* by default, since a shield that is always
      click-through blocks nothing.
    - **Setting the style from inside the low-level mouse hook does not
      help the very press that set it.** The style change itself lands
      promptly -- watched across a real press, the shield went
      pass-through 4ms in and the point under the cursor hit-tested to
      the taskbar -- but the press being handled is routed as though the
      shield were still closed. The same press 300ms later opens the
      jumplist. So the press has to be swallowed and a fresh one sent in
      its place.
    - **The replay has to come from the message loop, not the hook.** A
      replay issued from inside the callback fails exactly as the
      original press did; posted, and therefore sent after the hook has
      returned, it works. That is the whole difference.

    Two details the replay needs. It carries a signature in
    `MOUSEINPUT::dwExtraInfo` which the hook checks on the way in -- a
    replay mistaken for a real press would replay itself forever, inside
    a low-level hook. And only the *press* is replayed: by the time the
    release arrives the shield is already open, so the real one reaches
    the taskbar untouched, which is what keeps a press-and-drag onto a
    button one continuous gesture. The exception is a click faster than a
    message-loop turn, where the release is swallowed too and the replay
    carries it.

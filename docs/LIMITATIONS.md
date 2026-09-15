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
   actually be contained inside a group** — confirmed live, both
   `SetParent` and `SetWindowLongPtr(GWLP_HWNDPARENT)` fail outright with
   `ERROR_INVALID_PARAMETER` for the `ApplicationFrameWindow` class every
   time (mixed-DPI hosting or not, for the first; logged at join time as
   `[Polish] Attach: owner did NOT stick` for the second) — Windows
   refuses both embedding *and* ownership across this particular process
   boundary. Such a window still joins a group, but as an **attached**
   member rather than an embedded one: it stays an ordinary top-level
   window, and Polish drives everything an owner relationship would
   otherwise have provided for free, by hand:
   - **Z-order**: kept directly above the chrome (`SetWindowPos` with
     that as `hWndInsertAfter`) at every layout pass, and again whenever
     the chrome is brought forward (`GroupChromeWindow::SetOnZOrderChanged`
     → `GroupManager::RaiseAttachedMembers`) — clicking the group's title
     bar or Alt+Tabbing back to it. An unrelated window activated in
     between still correctly covers both, same as it would with a real
     owner relationship.
   - **Minimize/restore**: `ShowWindow(SW_HIDE)`/`SW_SHOWNOACTIVATE`
     driven off the chrome's own `WM_SIZE` transitions
     (`GroupChromeWindow::SetOnMinimizedChanged` →
     `GroupManager::SetAttachedMembersHidden`), rather than relying on
     Windows to hide/restore an owned window automatically.
   - **Candidate filtering**: since it's a plain top-level window (not
     owned, so `IsCandidateWindowShape`'s owned-window exclusion doesn't
     apply), the picker explicitly excludes every other group's members
     by hwnd (`GroupPickerWindow::ShowModal`'s `excludedWindows`) — an
     attached member that wasn't excluded this way could otherwise be
     offered to, and fought over by, a second group.
   - **Destroy-with-owner does *not* apply** — precisely because there is
     no real owner relationship, an attached member is *not* at risk of
     being destroyed if the group's chrome is destroyed abruptly (unlike
     an embedded member's `WS_CHILD` relationship, which is). Detaching
     it (`GroupManager::ReleaseGroup`/`ReleaseMember`, on every normal
     "close group"/remove-member path) is still done for cleanliness, but
     isn't load-bearing against data loss the way it is on the embedded
     side.
   - It keeps its own title bar/frame, drawn by its own process — Polish
     never strips it the way it does for an embedded member.
   - It is not clipped to the group's window; it floats above the
     chrome's own rect rather than being drawn inside it, so it can
     visibly overhang when the group is partly offscreen, resized
     smaller than it, or overlapped by another window.
   - It is hidden (not merely covered) when its tab isn't the active one
     in Tab mode — a plain top-level window can't be covered by a sibling
     the way an embedded member can.
   - **It keeps its own taskbar button, and every known mechanism has
     been tried.** All verified live against Calculator, all reporting
     success, none removing the button:
     - `WS_EX_TOOLWINDOW` — accepted, reads back as set (unlike
       `GWLP_HWNDPARENT`, which is refused outright), ignored by the
       taskbar. Also tried bracketed by the hide/show cycle that
       normally makes the shell re-evaluate taskbar membership after a
       style change.
     - `ITaskbarList::DeleteTab` — returns `S_OK`, re-applied on every
       layout pass in case the shell re-adds the button on show.
     - `IApplicationView::SetShowInSwitchers(FALSE)` — the shell's own
       internal interface, the one the virtual-desktop feature uses.
       Reached successfully (its vtable layout check passes on this
       build) and returns `S_OK`. The button still stays.

     A packaged app's taskbar button is evidently driven by its package
     identity via `ApplicationFrameHost` rather than by the window this
     app holds a handle to. All three calls are left in place: they cost
     nothing, and the first two are correct for any non-packaged window
     that ever takes the attached path.
   - It is excluded from Polish's *own* Alt+Tab, which is filtered on
     group membership directly rather than relying on any of the above.

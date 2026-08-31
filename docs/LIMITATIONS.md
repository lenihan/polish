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

# Known Limitations

These are permanent, by-design constraints of how Windows works — not bugs
to fix. This doc gets a full pass in Phase 3; today it records what's
already known from design.

1. **Custom-drawn title bars are unsupported by the visible button.**
   Chromium/Electron apps, WinUI3 apps (Settings, some Store apps), Windows
   Terminal, and VS Code (default titlebar mode) draw their own caption
   area, so there's no real minimize button to anchor the overlay to — it's
   skipped rather than guessed at. **The toggle itself still works on these
   windows via the global hotkey**, which doesn't depend on locating a
   button.

2. **Elevated (Run as Administrator) windows are inaccessible** to an
   unelevated instance of Polish, due to Windows' UIPI (User Interface
   Privilege Isolation) — both the button and the hotkey are blocked for
   these. This is the one case with no unelevated workaround. Use the tray
   menu's "Restart as Administrator" option if you want elevated windows
   covered too (this then controls all windows from an elevated process).

3. **Multi-monitor / mixed-DPI setups** are supported via Per-Monitor-V2
   DPI awareness, which depends on `app.manifest` being embedded correctly
   in the build.

4. **Toggle history is in-memory only** and resets when Polish restarts —
   it is not persisted across sessions.

5. **Event delivery is best-effort.** Windows can drop out-of-process
   WinEvent notifications under heavy system load; a low-frequency
   reconciliation sweep mitigates but doesn't eliminate rare staleness.

6. **A hung ("Not Responding") target window** can briefly (up to ~200ms)
   delay processing of other windows' events, but cannot hang Polish
   itself.

7. **The global hotkey always targets the current foreground window** —
   it has no way to address a specific background window.

# UIAccess build

While the Start menu (or Search) is open, Windows raises the taskbar into the
MOGO z-band, above Polish's normal shield window, and the native hover flyout
covers Polish's window list (LIMITATIONS #24). A process with a UIAccess token
gets its windows placed in the UIACCESS band, which is topmost of all, so the
shield keeps winning. Measured on build 26200.9457: with Start open the
taskbar is band 6, the UIAccess shield is band 2, and `WindowFromPoint` over a
button returns the shield.

## Two executables
- `polish.exe` -- `asInvoker`, runs from anywhere, used for development.
- `polish_uia.exe` -- same sources, manifest `uiAccess="true"`. Windows only
  launches it if it is Authenticode-signed by a certificate chaining to a
  trusted root AND lives under `%ProgramFiles%`.

The startup log says which mode is active ("UIAccess token: yes/no").

## Install / uninstall
    cmake --build build --config Release
    pwsh tools/uiaccess/Install-PolishUiAccess.ps1     # one UAC prompt
    & "$env:ProgramFiles\Polish\polish_uia.exe"
    pwsh tools/uiaccess/Uninstall-PolishUiAccess.ps1

Install creates a dedicated certificate (non-exportable key, CurrentUser\My),
trusts only its public half in LocalMachine\Root, and signs the copy in
Program Files. Start-at-login is unchanged unless you pass `-StartAtLogin`,
which points the existing Run value at the installed exe. Rebuilding requires
re-running Install to re-sign.

## What trusting the certificate means
Anything signed with that key can run with UIAccess on this machine. The key
cannot be exported, but remove the certificate with the uninstall script if
you stop using this build.

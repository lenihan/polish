# Installs the UIAccess build of Polish. Self-elevates (one UAC prompt).
#
#   1. Creates (once) a dedicated code-signing certificate whose private key
#      is NON-EXPORTABLE and lives in CurrentUser\My.
#   2. Trusts only its PUBLIC half in LocalMachine\Root -- Windows refuses
#      uiAccess="true" unless the signature chains to a trusted root.
#   3. Copies polish_uia.exe to %ProgramFiles%\Polish (a secure location is
#      also required) and signs it.
#
# Start-at-login is left alone unless you pass -StartAtLogin, which points
# the existing HKCU Run value "Polish" at the installed exe.
# Undo with Uninstall-PolishUiAccess.ps1. See docs/UIACCESS.md.
param([string]$Exe = (Join-Path $PSScriptRoot '..\..\build\Release\polish_uia.exe'), [switch]$StartAtLogin, [switch]$Elevated)
$ErrorActionPreference = 'Stop'
$Subject = 'CN=Polish UIAccess (dev, safe to delete)'
$Dir = Join-Path $env:ProgramFiles 'Polish'

if (-not ((whoami /groups) -match 'S-1-16-(12288|16384)')) {
    if ($Elevated) { throw 'Relaunched elevated but still not Administrator; aborting instead of relaunching again.' }
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Exe', (Resolve-Path $Exe).Path, '-Elevated') + $(if ($StartAtLogin) { '-StartAtLogin' })
    $p = Start-Process pwsh -Verb RunAs -Wait -PassThru -ArgumentList $argList
    exit $p.ExitCode
}

$Exe = (Resolve-Path $Exe).Path
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -KeyExportPolicy NonExportable `
        -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(2)
}
$cer = Join-Path $env:TEMP 'polish-uia.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Remove-Item $cer

Get-Process polish, polish_uia -ErrorAction Ignore | Stop-Process -Force
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
$target = Join-Path $Dir 'polish_uia.exe'
Copy-Item $Exe $target -Force
$sig = Set-AuthenticodeSignature -FilePath $target -Certificate $cert -HashAlgorithm SHA256
if ($sig.Status -ne 'Valid') { throw "signing failed: $($sig.Status) $($sig.StatusMessage)" }
if ($StartAtLogin) {
    Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name Polish -Value "`"$target`""
    Write-Host 'start-at-login now launches the UIAccess build'
}
Write-Host "Installed and signed: $target (thumbprint $($cert.Thumbprint))"

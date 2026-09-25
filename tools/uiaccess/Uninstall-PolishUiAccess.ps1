# Removes what Install-PolishUiAccess.ps1 added: the installed exe and the
# certificate in LocalMachine\Root and CurrentUser\My. Self-elevates.
param([switch]$Elevated)
$ErrorActionPreference = 'Stop'
$Subject = 'CN=Polish UIAccess (dev, safe to delete)'
if (-not ((whoami /groups) -match 'S-1-16-(12288|16384)')) {
    if ($Elevated) { throw 'Relaunched elevated but still not Administrator; aborting instead of relaunching again.' }
    $p = Start-Process pwsh -Verb RunAs -Wait -PassThru -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Elevated'
    exit $p.ExitCode
}
Get-Process polish_uia -ErrorAction Ignore | Stop-Process -Force
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if ((Get-ItemProperty $run -Name Polish -ErrorAction Ignore).Polish -like '*\Polish\polish_uia.exe*') { Remove-ItemProperty $run -Name Polish }
Remove-Item (Join-Path $env:ProgramFiles 'Polish') -Recurse -Force -ErrorAction Ignore
foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\CurrentUser\My') {
    Get-ChildItem $store | Where-Object { $_.Subject -eq $Subject } | Remove-Item -Force
}
Write-Host 'Uninstalled.'

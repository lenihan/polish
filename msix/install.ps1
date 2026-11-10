<#
.SYNOPSIS
  Installs the test MSIX on this PC.

  -Register   Developer Mode is on: register the staged layout directly. No
              certificate, and the files stay editable in msix\layout.
  (default)   Run elevated: trusts the test certificate and installs the .msix.
  -Remove     Uninstalls it.
#>
param([switch]$Register, [switch]$Remove, [string]$Name = 'Polish.Dev')
$ErrorActionPreference = 'Stop'
$dist = Join-Path $PSScriptRoot '..\dist'
if ($Remove) { Get-AppxPackage $Name | Remove-AppxPackage; return }
Get-Process polish -ErrorAction SilentlyContinue | Stop-Process -Force
if ($Register) {
    Add-AppxPackage -Register (Join-Path $PSScriptRoot 'layout\AppxManifest.xml')
} else {
    Import-Certificate -FilePath (Join-Path $dist 'Polish-test.cer') -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
    Add-AppxPackage (Get-ChildItem $dist -Filter 'Polish-*-x64.msix' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
Get-AppxPackage $Name | Select-Object Name, Version, InstallLocation

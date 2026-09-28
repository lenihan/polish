# Builds the MSI that gets attached to a GitHub Release.
#
# Defaults to the plain polish.exe, which needs no code signature. Once a
# code-signing certificate exists, pass -UiAccess to ship polish_uia.exe
# instead and users get the shield's Start-menu-open fix -- Windows refuses
# to launch a uiAccess executable that is not signed, so that switch and the
# certificate arrive together or not at all.
#
# Requires the WiX dotnet tool:  dotnet tool install --global wix
#
#   pwsh installer/build.ps1                 # x64 MSI from build-x64
#   pwsh installer/build.ps1 -UiAccess        # once signing exists
param(
    [switch]$UiAccess,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-x64\Release'),
    [string]$OutDir   = (Join-Path $PSScriptRoot '..\dist')
)
$ErrorActionPreference = 'Stop'

$repo = Resolve-Path (Join-Path $PSScriptRoot '..')

# The version lives in CMakeLists.txt's project() call and nowhere else; the
# MSI reads it from there rather than keeping a second copy to drift.
$cmake = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(polish\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not read VERSION from CMakeLists.txt project() call'
}
$version = $Matches[1]

$exeName = if ($UiAccess) { 'polish_uia.exe' } else { 'polish.exe' }
$exePath = Join-Path (Resolve-Path $BuildDir) $exeName
if (-not (Test-Path $exePath)) {
    throw "$exeName not found in $BuildDir -- build it first: cmake --build --preset x64"
}

# The one check that matters for a file other people will run: an ARM64
# binary does not run on an ordinary PC at all, and the build follows
# whatever machine produced it unless the x64 preset was used.
$bytes = [System.IO.File]::ReadAllBytes($exePath)
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne 0x8664) {
    throw ("{0} is not x64 (PE machine 0x{1:X}). Build with: cmake --build --preset x64" -f $exeName, $machine)
}

if ($UiAccess) {
    $sig = Get-AuthenticodeSignature $exePath
    if ($sig.Status -ne 'Valid') {
        throw "$exeName is not validly signed ($($sig.Status)). Windows will refuse to launch a uiAccess build."
    }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$msi = Join-Path (Resolve-Path $OutDir) "Polish-$version-x64.msi"

& wix build (Join-Path $PSScriptRoot 'Polish.wxs') `
    -arch x64 `
    -define "Version=$version" `
    -define "ExePath=$exePath" `
    -define "IconPath=$(Join-Path $repo 'resources\polish.ico')" `
    -define "LicensePath=$(Join-Path $repo 'LICENSE')" `
    -out $msi
if ($LASTEXITCODE -ne 0) { throw "wix build failed with exit code $LASTEXITCODE" }

Write-Host ""
Write-Host "Built $msi"
Write-Host "  version   $version"
Write-Host "  payload   $exeName (x64)"
Write-Host "  signed    $(if ($UiAccess) { 'yes -- UIAccess build' } else { 'no -- expect a SmartScreen warning' })"

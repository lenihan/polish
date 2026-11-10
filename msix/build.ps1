<#
.SYNOPSIS
  Builds an MSIX of Polish for local testing (and, with the Store's identity,
  for submission).

.DESCRIPTION
  Stages polish.exe, the manifest and the logos in msix\layout, packs them
  with makeappx, and signs the package. Signing uses a self-signed test
  certificate created on first use (CurrentUser\My) unless -PfxPath is given.
  The Store signs a submitted package itself, so the certificate only matters
  for sideloading while testing.

  To run the result:
    - Developer Mode on:  pwsh msix\install.ps1 -Register     (no certificate needed)
    - otherwise, elevated: pwsh msix\install.ps1               (trusts the test certificate, installs the .msix)

.EXAMPLE
  cmake --build --preset x64; powershell -File msix\build.ps1
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-x64\Release'),
    [string]$OutDir   = (Join-Path $PSScriptRoot '..\dist'),
    [string]$Name = 'Polish.Dev',
    [string]$Publisher = 'CN=Polish Test',
    [string]$PublisherDisplay = 'Polish (test)',
    [string]$PfxPath,
    [string]$PfxPassword
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$cmake = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(polish\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { throw 'No VERSION in CMakeLists.txt' }
$version = "$($Matches[1]).0"

$kits = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory | Where-Object { $_.Name -match '^10\.' } |
    Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'x64' } |
    Where-Object { Test-Path (Join-Path $_ 'makeappx.exe') } | Select-Object -First 1
if (-not $kits) { throw 'makeappx.exe not found: install the Windows SDK' }
$makeappx = Join-Path $kits 'makeappx.exe'
$signtool = Join-Path $kits 'signtool.exe'

$exe = Join-Path (Resolve-Path $BuildDir) 'polish.exe'
if (-not (Test-Path $exe)) { throw "polish.exe not found in $BuildDir -- build it first: cmake --build --preset x64" }

# Stage.
$layout = Join-Path $PSScriptRoot 'layout'
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $layout 'Assets') | Out-Null
Copy-Item $exe $layout
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $layout 'LICENSE.txt')

# Logos: the largest image in polish.ico, resized.
$bytes = [IO.File]::ReadAllBytes((Join-Path $repo 'resources\polish.ico'))
$count = [BitConverter]::ToUInt16($bytes, 4)
$best = 0; $bestSize = 0
for ($i = 0; $i -lt $count; $i++) {
    $o = 6 + 16 * $i; $w = $bytes[$o]; if ($w -eq 0) { $w = 256 }
    if ($w -gt $bestSize) { $best = $i; $bestSize = $w }
}
$o = 6 + 16 * $best
$len = [BitConverter]::ToUInt32($bytes, $o + 8); $off = [BitConverter]::ToUInt32($bytes, $o + 12)
$src = [Drawing.Image]::FromStream((New-Object IO.MemoryStream (, $bytes[$off..($off + $len - 1)])))
foreach ($logo in @(@('StoreLogo', 50), @('Square44x44Logo', 44), @('Square150x150Logo', 150))) {
    $size = $logo[1]
    $bmp = New-Object Drawing.Bitmap $size, $size
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.Clear([Drawing.Color]::Transparent)
    $pad = [int]($size * 0.08)
    $g.DrawImage($src, $pad, $pad, $size - 2 * $pad, $size - 2 * $pad)
    $g.Dispose()
    $bmp.Save((Join-Path $layout "Assets\$($logo[0]).png"), [Drawing.Imaging.ImageFormat]::Png)
}

# Manifest.
$manifest = Get-Content (Join-Path $PSScriptRoot 'AppxManifest.xml') -Raw
$manifest = $manifest.Replace('@NAME@', $Name).Replace('@PUBLISHER@', $Publisher).Replace('@VERSION@', $version).Replace('@PUBLISHERDISPLAY@', $PublisherDisplay)
Set-Content -Path (Join-Path $layout 'AppxManifest.xml') -Value $manifest -Encoding UTF8

# Pack.
New-Item -ItemType Directory -Force $OutDir | Out-Null
$msix = Join-Path (Resolve-Path $OutDir) "Polish-$version-x64.msix"
& $makeappx pack /d $layout /p $msix /o | Out-Host
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }

# Sign (test certificate unless given one).
if (-not $PfxPath) {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Publisher -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
            -FriendlyName 'Polish test signing' -CertStoreLocation Cert:\CurrentUser\My `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    }
    Export-Certificate -Cert $cert -FilePath (Join-Path (Resolve-Path $OutDir) 'Polish-test.cer') | Out-Null
    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $msix | Out-Host
} else {
    & $signtool sign /fd SHA256 /f $PfxPath /p $PfxPassword $msix | Out-Host
}
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
Write-Host "Built $msix"

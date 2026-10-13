<#
.SYNOPSIS
  Draws Polish's icon candidates procedurally and writes PNGs, a contact sheet
  and .ico files; optionally installs one as resources\polish.ico.

.DESCRIPTION
  The icon is a window pane with a title bar and a bold diagonal gloss streak
  across the glass -- "a polished window". Deliberately no stars or sparkles.

  Variants:
    A  blue glass, one white streak
    B  graphite glass, one cyan-white streak (reads on light taskbars)
    C  blue glass, one thick and one thin streak

  Small sizes snap their geometry to whole pixels and drop the title-bar
  detail they cannot hold, because an icon that is a smudge at 16 px is the
  one that is actually seen most.

.EXAMPLE
  pwsh tools\make-icon.ps1                 # candidates + contact sheet
  pwsh tools\make-icon.ps1 -Install B      # overwrite resources\polish.ico
#>
param(
    [ValidateSet('A', 'B', 'C')][string]$Install,
    [string]$OutDir = (Join-Path $PSScriptRoot 'icon-out')
)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$Sizes = 16, 20, 24, 32, 40, 48, 64, 256
$Variants = 'A', 'B', 'C'
$Repo = Split-Path -Parent $PSScriptRoot

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-Streak([single]$cx, [single]$cy, [single]$length, [single]$offset, [single]$width) {
    # A band running bottom-left to top-right through (cx, cy), shifted
    # `offset` along its normal, `width` thick.
    $k = [single]0.70710678
    $dx = $k; $dy = -$k          # along the band
    $nx = $k; $ny = $k           # across it
    $ox = $cx + $nx * $offset; $oy = $cy + $ny * $offset
    $h = $width / 2
    [System.Drawing.PointF[]]$points = @(
        (New-Object System.Drawing.PointF ($ox + $dx * $length + $nx * $h), ($oy + $dy * $length + $ny * $h)),
        (New-Object System.Drawing.PointF ($ox - $dx * $length + $nx * $h), ($oy - $dy * $length + $ny * $h)),
        (New-Object System.Drawing.PointF ($ox - $dx * $length - $nx * $h), ($oy - $dy * $length - $ny * $h)),
        (New-Object System.Drawing.PointF ($ox + $dx * $length - $nx * $h), ($oy + $dy * $length - $ny * $h))
    )
    return , $points
}

function Draw-Icon([int]$S, [string]$V) {
    $bmp = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [Math]::Max(1, [Math]::Round($S / 16.0))
    $x = $m; $y = $m; $w = $S - 2 * $m; $h = $S - 2 * $m
    $r = [Math]::Max(2, [Math]::Round($S * 0.17))

    if ($V -eq 'B') {
        $top = [System.Drawing.Color]::FromArgb(255, 0x3E, 0x4C, 0x66)
        $bottom = [System.Drawing.Color]::FromArgb(255, 0x14, 0x1B, 0x2B)
        $bar = [System.Drawing.Color]::FromArgb(255, 0x0A, 0x0F, 0x1A)
        $streak = [System.Drawing.Color]::FromArgb(240, 0x8A, 0xE6, 0xFF)
    } else {
        $top = [System.Drawing.Color]::FromArgb(255, 0x56, 0xB4, 0xFF)
        $bottom = [System.Drawing.Color]::FromArgb(255, 0x0B, 0x62, 0xC9)
        $bar = [System.Drawing.Color]::FromArgb(255, 0x07, 0x45, 0x96)
        $streak = [System.Drawing.Color]::FromArgb(240, 255, 255, 255)
    }

    $path = New-RoundedPath $x $y $w $h $r
    $rect = New-Object System.Drawing.RectangleF $x, $y, $w, $h
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $top, $bottom, 90.0
    $g.FillPath($fill, $path)
    $g.SetClip($path)

    # Title bar.
    $barH = [Math]::Max(3, [Math]::Round($S * 0.22))
    $barBrush = New-Object System.Drawing.SolidBrush $bar
    $g.FillRectangle($barBrush, $x, $y, $w, $barH)
    if ($S -ge 24) {
        $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 255, 255, 255))
        $dot = [Math]::Max(1.5, $S * 0.05)
        $g.FillEllipse($white, $x + $S * 0.11, $y + $barH / 2 - $dot, $dot * 2, $dot * 2)
        $lineH = [Math]::Max(1, [Math]::Round($S * 0.045))
        $g.FillRectangle($white, $x + $S * 0.27, $y + $barH / 2 - $lineH / 2, $S * 0.34, $lineH)
    }

    # The gloss, clipped to the glass below the bar.
    $g.IntersectClip((New-Object System.Drawing.RectangleF $x, ($y + $barH), $w, ($h - $barH)))
    $streakBrush = New-Object System.Drawing.SolidBrush $streak
    $cx = $x + $w / 2 - $S * 0.04
    $cy = $y + $barH + ($h - $barH) / 2
    $length = $S * 2
    $thick = if ($V -eq 'C') { $S * 0.17 } else { $S * 0.21 }
    $g.FillPolygon($streakBrush, (New-Streak $cx $cy $length 0 $thick))
    if ($V -eq 'C') {
        $thin = [Math]::Max(1.5, $S * 0.05)
        $gap = $S * 0.06
        $g.FillPolygon($streakBrush, (New-Streak $cx $cy $length ($thick / 2 + $gap + $thin / 2) $thin))
    }

    $g.Dispose()
    return $bmp
}

function Get-PngBytes($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return , $ms.ToArray()
}

function Write-Ico([byte[][]]$pngs, [int[]]$sizes, [string]$path) {
    # ICONDIR + one 16-byte entry per image + the PNG payloads. PNG entries
    # are what the current polish.ico uses and are fine from Vista on.
    $stream = [System.IO.File]::Create($path)
    $w = New-Object System.IO.BinaryWriter $stream
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$pngs.Count)
    $offset = 6 + 16 * $pngs.Count
    for ($i = 0; $i -lt $pngs.Count; $i++) {
        $s = $sizes[$i]
        $dim = if ($s -ge 256) { [byte]0 } else { [byte]$s }
        $w.Write($dim); $w.Write($dim)
        $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32)
        $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
        $offset += $pngs[$i].Length
    }
    foreach ($png in $pngs) { $w.Write($png) }
    $w.Dispose(); $stream.Dispose()
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

# Candidates: PNGs per size, and a .ico per variant.
$images = @{}
foreach ($v in $Variants) {
    $pngs = @(); $list = @()
    foreach ($s in $Sizes) {
        $bmp = Draw-Icon $s $v
        $file = Join-Path $OutDir ("polish-{0}-{1}.png" -f $v, $s)
        $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        $images["$v$s"] = $bmp
        $pngs += , (Get-PngBytes $bmp)
        $list += $s
    }
    Write-Ico $pngs $list (Join-Path $OutDir "polish-$v.ico")
}

# Contact sheet: the current icon next to each candidate, on a light and a
# dark taskbar-coloured ground, at 1x and 4x (nearest neighbour, so the
# pixels are the pixels).
$currentPath = Join-Path $Repo 'resources\polish.ico'
function Get-CurrentBitmap([int]$s) {
    $icon = New-Object System.Drawing.Icon $currentPath, $s, $s
    return $icon.ToBitmap()
}
$rows = @('Current') + $Variants
$cell = 150
$sheet = New-Object System.Drawing.Bitmap (2 * 520 + 20), ($rows.Count * $cell + 10)
$sg = [System.Drawing.Graphics]::FromImage($sheet)
$sg.Clear([System.Drawing.Color]::FromArgb(255, 128, 128, 128))
$font = New-Object System.Drawing.Font 'Segoe UI', 10
$grounds = @(
    @{ x = 5;   color = [System.Drawing.Color]::FromArgb(255, 243, 243, 243); text = [System.Drawing.Brushes]::Black },
    @{ x = 535; color = [System.Drawing.Color]::FromArgb(255, 32, 32, 32);    text = [System.Drawing.Brushes]::White }
)
for ($row = 0; $row -lt $rows.Count; $row++) {
    $name = $rows[$row]
    $top = 5 + $row * $cell
    foreach ($ground in $grounds) {
        $sg.FillRectangle((New-Object System.Drawing.SolidBrush $ground.color), $ground.x, $top, 510, $cell - 5)
        $sg.DrawString($name, $font, $ground.text, $ground.x + 8, $top + 6)
        $px = $ground.x + 10
        foreach ($s in 16, 24, 32, 48) {
            $bmp = if ($name -eq 'Current') { Get-CurrentBitmap $s } else { $images["$name$s"] }
            $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
            $sg.DrawImage($bmp, $px, $top + 40, $s, $s)
            $px += $s + 14
        }
        # 4x of the 32 px image, so the hinting can be judged.
        $big = if ($name -eq 'Current') { Get-CurrentBitmap 32 } else { $images["${name}32"] }
        $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $sg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $sg.DrawImage($big, $px + 10, $top + 8, 128, 128)
    }
}
$sg.Dispose()
$sheetPath = Join-Path $OutDir 'contact-sheet.png'
$sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "Wrote candidates and $sheetPath"

if ($Install) {
    Copy-Item (Join-Path $OutDir "polish-$Install.ico") $currentPath -Force
    Write-Host "Installed variant $Install as $currentPath"
}

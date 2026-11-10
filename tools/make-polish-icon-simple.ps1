<#
.SYNOPSIS
  Draws simpler candidates for the Polish icon -- one window card, the same
  card the stack icon uses as its front layer -- and writes PNGs, a contact
  sheet and .ico files; optionally installs one as resources\polish.ico.

.DESCRIPTION
  Variants (all: blue glass card, darker title bar, same colours as the stack icon):
    A  the bare card -- nothing else
    B  the card with one soft gloss band across the glass
    C  the card with a small four-point sparkle in the glass
    D  the card with three window-control dots in the title bar

.EXAMPLE
  pwsh tools\make-polish-icon-simple.ps1                 # candidates + contact sheet
  pwsh tools\make-polish-icon-simple.ps1 -Install B      # overwrite resources\polish.ico
#>
param(
    [ValidateSet('A', 'B', 'C', 'D')][string]$Install,
    [string]$OutDir = (Join-Path $PSScriptRoot 'icon-out')
)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$Sizes = 16, 20, 24, 32, 40, 48, 64, 256
$Variants = 'A', 'B', 'C', 'D'
$Repo = Split-Path -Parent $PSScriptRoot

function Html($c) { [System.Drawing.ColorTranslator]::FromHtml($c) }

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

function New-Sparkle([single]$cx, [single]$cy, [single]$r) {
    # A four-point star with concave sides.
    $k = 0.22
    [System.Drawing.PointF[]]$pts = @(
        (New-Object System.Drawing.PointF $cx, ($cy - $r)),
        (New-Object System.Drawing.PointF ($cx + $r * $k), ($cy - $r * $k)),
        (New-Object System.Drawing.PointF ($cx + $r), $cy),
        (New-Object System.Drawing.PointF ($cx + $r * $k), ($cy + $r * $k)),
        (New-Object System.Drawing.PointF $cx, ($cy + $r)),
        (New-Object System.Drawing.PointF ($cx - $r * $k), ($cy + $r * $k)),
        (New-Object System.Drawing.PointF ($cx - $r), $cy),
        (New-Object System.Drawing.PointF ($cx - $r * $k), ($cy - $r * $k))
    )
    return , $pts
}

function Draw-Icon([int]$S, [string]$V, [string]$Theme = 'dark') {
    $bmp = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [Math]::Max(1, [Math]::Round($S / 16.0))
    $x = $m; $y = $m; $w = $S - 2 * $m; $h = $S - 2 * $m
    $r = [Math]::Max(2, [Math]::Round($S * 0.15))
    # Small sizes (the tray, the taskbar) are drawn to hold up on both a dark
    # and a light taskbar. A dark title bar vanishes into a dark taskbar and a
    # pale body into a light one, so: a bright mid-blue bar and a light body,
    # which carry the icon on dark, and a 1 px dark-blue outline and rule under
    # the bar, which carry it on light. Neither is a stripe at 16 px.
    $small = $S -le 32
    $barH = if ($small) { [Math]::Max(3, [Math]::Round($S * 0.25)) } else { [Math]::Max(3, [Math]::Round($S * 0.2)) }
    $outline = if ($Theme -eq 'dark') { '#FFFFFF' } else { '#202020' }

    $path = New-RoundedPath $x $y $w $h $r
    $rect = New-Object System.Drawing.RectangleF $x, $y, $w, $h
    $top = if ($small) { '#3D9BF8' } else { '#56B4FF' }
    $bottom = if ($small) { '#0F66D6' } else { '#0B62C9' }
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, (Html $top), (Html $bottom), 90.0
    $g.FillPath($fill, $path)
    $g.SetClip($path)
    $barColor = if ($Theme -eq 'dark') { '#FFFFFF' } else { '#074596' }
    $g.FillRectangle((New-Object System.Drawing.SolidBrush (Html $barColor)), $x, $y, $w, $barH)

    if ($V -eq 'D' -and $S -ge 24) {
        $dot = [Math]::Max(1.6, $S * 0.045)
        $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(235, 255, 255, 255))
        for ($i = 0; $i -lt 3; $i++) {
            $g.FillEllipse($white, $x + $S * 0.1 + $i * $S * 0.12, $y + $barH / 2 - $dot, $dot * 2, $dot * 2)
        }
    }

    $glassTop = $y + $barH
    $g.IntersectClip((New-Object System.Drawing.RectangleF $x, $glassTop, $w, ($h - $barH)))

    if ($V -eq 'B') {
        # One soft band, lower-left to upper-right.
        $k = 0.70710678
        $cx = $x + $w * 0.5; $cy = $glassTop + ($h - $barH) * 0.55
        $len = $S * 2; $half = $S * 0.12
        $nx = $k; $ny = $k; $dx = $k; $dy = -$k
        [System.Drawing.PointF[]]$band = @(
            (New-Object System.Drawing.PointF ($cx + $dx * $len + $nx * $half), ($cy + $dy * $len + $ny * $half)),
            (New-Object System.Drawing.PointF ($cx - $dx * $len + $nx * $half), ($cy - $dy * $len + $ny * $half)),
            (New-Object System.Drawing.PointF ($cx - $dx * $len - $nx * $half), ($cy - $dy * $len - $ny * $half)),
            (New-Object System.Drawing.PointF ($cx + $dx * $len - $nx * $half), ($cy + $dy * $len - $ny * $half))
        )
        $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(150, 255, 255, 255))), $band)
    }

    if ($V -eq 'C') {
        $sr = $S * 0.27
        $star = New-Sparkle ($x + $w * 0.5) ($glassTop + ($h - $barH) * 0.5) $sr
        $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(245, 255, 255, 255))), $star)
    }

    # The outline, every size: white with a white title bar for a dark taskbar,
    # near-black with a dark bar for a light one. Drawn last and unclipped,
    # inset half its width so the whole stroke lands inside the card.
    $lw = [Math]::Max(1, [Math]::Round($S / 32.0))
    $g.ResetClip()
    $ringPath = New-RoundedPath ($x + $lw / 2) ($y + $lw / 2) ($w - $lw) ($h - $lw) ([Math]::Max(1.5, $r - $lw / 2))
    $g.DrawPath((New-Object System.Drawing.Pen (Html $outline), $lw), $ringPath)

    $g.Dispose()
    return $bmp
}

function Get-PngBytes($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return , $ms.ToArray()
}

function Write-Ico([byte[][]]$pngs, [int[]]$sizes, [string]$path) {
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
$images = @{}
foreach ($v in $Variants) {
    foreach ($theme in 'dark', 'light') {
        $pngs = @()
        foreach ($s in $Sizes) {
            $bmp = Draw-Icon $s $v $theme
            if ($theme -eq 'dark') { $images["$v$s"] = $bmp }
            $pngs += , (Get-PngBytes $bmp)
        }
        $suffix = if ($theme -eq 'dark') { '' } else { '-light' }
        Write-Ico $pngs $Sizes (Join-Path $OutDir "polish-simple-$v$suffix.ico")
    }
}

# Contact sheet: the current Polish icon and the stack icon for reference,
# then each candidate, on light and dark taskbar grounds at 16/24/32/48, plus
# a 4x of the 32 px image.
function Get-IconBitmap([string]$path, [int]$s) { try { (New-Object System.Drawing.Icon $path, $s, $s).ToBitmap() } catch { New-Object System.Drawing.Bitmap $s, $s } }
$rows = @('Current', 'Stack') + $Variants
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
        $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $sg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        foreach ($s in 16, 24, 32, 48) {
            $bmp = switch ($name) {
                'Current' { Get-IconBitmap (Join-Path $Repo 'resources\polish.ico') $s }
                'Stack'   { Get-IconBitmap (Join-Path $Repo 'resources\stack.ico') $s }
                default   { $images["$name$s"] }
            }
            $sg.DrawImage($bmp, $px, $top + 40, $s, $s)
            $px += $s + 14
        }
        $big = switch ($name) {
            'Current' { Get-IconBitmap (Join-Path $Repo 'resources\polish.ico') 32 }
            'Stack'   { Get-IconBitmap (Join-Path $Repo 'resources\stack.ico') 32 }
            default   { $images["${name}32"] }
        }
        $sg.DrawImage($big, $px + 10, $top + 8, 128, 128)
    }
}
$sg.Dispose()
$sheetPath = Join-Path $OutDir 'polish-simple-contact-sheet.png'
$sheet.Save($sheetPath, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "Wrote candidates and $sheetPath"

if ($Install) {
    $dest = Join-Path $Repo 'resources\polish.ico'
    Copy-Item (Join-Path $OutDir "polish-simple-$Install.ico") $dest -Force
    $destLight = Join-Path $Repo 'resources\polish-light.ico'
    Copy-Item (Join-Path $OutDir "polish-simple-$Install-light.ico") $destLight -Force
    Write-Host "Installed variant $Install as $dest and $destLight"
}

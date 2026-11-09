<#
.SYNOPSIS
  Draws the stack icon -- the one a stack's taskbar button shows -- and writes
  resources\stack.ico plus a preview sheet.

.DESCRIPTION
  Three cascaded window cards, front one fullest: the same blue glass and
  title bar as the Polish icon, so it reads as family, but with the one thing
  that says "stack" and nothing else. No gloss streak: at 16 px the offset
  cards are all the detail there is room for.

  Small sizes snap to whole pixels, and drop the hairline edge between cards,
  which at 16 px would eat the cards themselves.

.EXAMPLE
  pwsh tools\make-stack-icon.ps1
#>
param([string]$OutDir = (Join-Path $PSScriptRoot 'icon-out'))

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$Sizes = 16, 20, 24, 32, 40, 48, 64, 256
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

function Draw-StackIcon([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [Math]::Max(1, [Math]::Round($S / 16.0))
    $step = [Math]::Max(2, [Math]::Round($S * 0.13))
    $cardW = $S - 2 * $m - 2 * $step
    $cardH = $S - 2 * $m - 2 * $step
    $r = [Math]::Max(1.5, [Math]::Round($S * 0.11))
    $barH = [Math]::Max(2, [Math]::Round($S * 0.17))

    # Back to front. Each layer: its bar colour, and its body colours.
    $layers = @(
        @{ i = 0; bar = '#5FA3E8'; top = '#BFE0FF'; bottom = '#9CCBFF' },
        @{ i = 1; bar = '#2F7FD6'; top = '#8CCBFF'; bottom = '#5FAEF5' },
        @{ i = 2; bar = '#074596'; top = '#56B4FF'; bottom = '#0B62C9' }
    )
    foreach ($layer in $layers) {
        # Cascade: the back card sits up and to the right, the front down and left.
        $x = $m + (2 - $layer.i) * $step
        $y = $m + $layer.i * $step
        $path = New-RoundedPath $x $y $cardW $cardH $r
        $rect = New-Object System.Drawing.RectangleF $x, $y, $cardW, $cardH
        $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect,
            ([System.Drawing.ColorTranslator]::FromHtml($layer.top)),
            ([System.Drawing.ColorTranslator]::FromHtml($layer.bottom)), 90.0
        $g.FillPath($fill, $path)

        $state = $g.Save()
        $g.SetClip($path)
        $g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($layer.bar))),
            $x, $y, $cardW, $barH)
        $g.Restore($state)

        # A hairline edge in the card's own bar colour keeps the layers apart
        # where they overlap -- affordable only once there are pixels for it.
        if ($S -ge 32) {
            $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200,
                ([System.Drawing.ColorTranslator]::FromHtml($layer.bar)).R,
                ([System.Drawing.ColorTranslator]::FromHtml($layer.bar)).G,
                ([System.Drawing.ColorTranslator]::FromHtml($layer.bar)).B)), ([Math]::Max(1, $S / 64.0))
            $g.DrawPath($pen, $path)
        }
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
$pngs = @(); $images = @{}
foreach ($s in $Sizes) {
    $bmp = Draw-StackIcon $s
    $images[$s] = $bmp
    $pngs += , (Get-PngBytes $bmp)
}
$icoPath = Join-Path $Repo 'resources\stack.ico'
Write-Ico $pngs $Sizes $icoPath

# Preview: light and dark taskbar-coloured grounds, 16/24/32/48 and a 4x of 32.
$sheet = New-Object System.Drawing.Bitmap 520, 160
$sg = [System.Drawing.Graphics]::FromImage($sheet)
$sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$sg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
foreach ($ground in @(@{ x = 0; c = '#F3F3F3' }, @{ x = 260; c = '#202020' })) {
    $sg.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($ground.c))), $ground.x, 0, 260, 160)
    $px = $ground.x + 10
    foreach ($s in 16, 24, 32, 48) { $sg.DrawImage($images[$s], $px, 20, $s, $s); $px += $s + 8 }
    $sg.DrawImage($images[64], $ground.x + 10, 80, 64, 64)
    $sg.DrawImage($images[256], $ground.x + 100, 40, 110, 110)
}
$sg.Dispose()
$sheet.Save((Join-Path $OutDir 'stack-preview.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "Wrote $icoPath and stack-preview.png"

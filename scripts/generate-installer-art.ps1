$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$output = Join-Path $PSScriptRoot '../ide/src-tauri/installer'
New-Item -ItemType Directory -Force $output | Out-Null

function New-Art([int]$width, [int]$height) {
    $bitmap = [System.Drawing.Bitmap]::new($width, $height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $canvas = [System.Drawing.Graphics]::FromImage($bitmap)
    $canvas.SmoothingMode = 'AntiAlias'
    $canvas.TextRenderingHint = 'AntiAliasGridFit'
    return @($bitmap, $canvas)
}
function Brush([string]$hex) { [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml($hex)) }
function Label($canvas, [string]$text, [float]$size, [float]$x, [float]$y, [string]$color, [bool]$bold = $false) {
    $style = if ($bold) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular }
    $font = [System.Drawing.Font]::new('Segoe UI', $size, $style, [System.Drawing.GraphicsUnit]::Pixel)
    $brush = Brush $color
    $canvas.DrawString($text, $font, $brush, $x, $y)
    $brush.Dispose(); $font.Dispose()
}
function Crab($canvas, [float]$left, [float]$top, [float]$unit) {
    $brush = Brush '#FF9820'
    $rows = @('▄▄          ▄▄', '▄▀██▄▀██▀▄██▀▄', '   ▀██████▀', '    ▄▀▀▀▀▄')
    for ($y = 0; $y -lt $rows.Count; $y++) {
        for ($x = 0; $x -lt $rows[$y].Length; $x++) {
            $glyph = $rows[$y][$x]
            if ($glyph -eq ' ') { continue }
            $offset = if ($glyph -eq '▄') { $unit } else { 0 }
            $height = if ($glyph -eq '█') { 2 * $unit } else { $unit }
            $canvas.FillRectangle($brush, $left + $x * $unit, $top + $y * 2 * $unit + $offset, $unit, $height)
        }
    }
    $brush.Dispose()
}

$bitmap, $canvas = New-Art 328 628
$canvas.Clear([System.Drawing.ColorTranslator]::FromHtml('#101820'))
$pen = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#1C2A35'), 1)
for ($i = 32; $i -lt 328; $i += 32) { $canvas.DrawLine($pen, $i, 0, $i, 628) }
for ($i = 20; $i -lt 628; $i += 32) { $canvas.DrawLine($pen, 0, $i, 328, $i) }
$pen.Dispose()
Label $canvas 'ARN / DESKTOP' 17 32 32 '#A8BBC8' $true
Crab $canvas 52 150 16
Label $canvas 'ARN IDE' 42 28 328 '#FFFFFF' $true
Label $canvas 'Your code.' 23 30 393 '#DCE6EC'
Label $canvas 'Your next idea.' 23 30 425 '#DCE6EC'
$accent = Brush '#FF9820'
$canvas.FillRectangle($accent, 32, 521, 48, 4)
$accent.Dispose()
Label $canvas 'CREATE WITH ARNY' 14 30 551 '#A8BBC8' $true
$bitmap.Save((Join-Path $output 'sidebar.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Save((Join-Path $output 'sidebar.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$canvas.Dispose(); $bitmap.Dispose()

$bitmap, $canvas = New-Art 150 57
$canvas.Clear([System.Drawing.Color]::White)
Crab $canvas 96 16 3
Label $canvas 'ARN IDE' 16 8 17 '#101820' $true
$bitmap.Save((Join-Path $output 'header.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$canvas.Dispose(); $bitmap.Dispose()

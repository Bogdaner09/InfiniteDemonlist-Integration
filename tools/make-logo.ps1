param(
    [string]$Resources = "C:\Program Files (x86)\Steam\steamapps\content\app_322170\depot_322171\Resources",
    [string]$Out = "C:\dev\integrated-idl\logo.png"
)
Add-Type -AssemblyName System.Drawing

$size = 336
$bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.InterpolationMode = 'HighQualityBicubic'
$g.PixelOffsetMode = 'HighQuality'
$g.Clear([System.Drawing.Color]::Transparent)

function RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Outer dark outline, gold border, then the red-to-black gradient inside.
$outer = RoundedRect 2 2 ($size - 4) ($size - 4) 54
$g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 20, 6, 10))), $outer)

$borderRect = New-Object System.Drawing.RectangleF 6, 6, ($size - 12), ($size - 12)
$border = RoundedRect 6 6 ($size - 12) ($size - 12) 50
$gold = New-Object System.Drawing.Drawing2D.LinearGradientBrush $borderRect, ([System.Drawing.Color]::FromArgb(255, 255, 226, 110)), ([System.Drawing.Color]::FromArgb(255, 214, 120, 16)), 90
$g.FillPath($gold, $border)

$innerRect = New-Object System.Drawing.RectangleF 18, 18, ($size - 36), ($size - 36)
$inner = RoundedRect 18 18 ($size - 36) ($size - 36) 40
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush $innerRect, ([System.Drawing.Color]::Black), ([System.Drawing.Color]::Black), 90
$blend = New-Object System.Drawing.Drawing2D.ColorBlend 3
$blend.Colors = @(
    [System.Drawing.Color]::FromArgb(255, 236, 64, 40),
    [System.Drawing.Color]::FromArgb(255, 140, 14, 40),
    [System.Drawing.Color]::FromArgb(255, 38, 4, 26)
)
$blend.Positions = @(0.0, 0.5, 1.0)
$bg.InterpolationColors = $blend
$g.FillPath($bg, $inner)

# Soft glow behind the face.
$glow = New-Object System.Drawing.Drawing2D.GraphicsPath
$glow.AddEllipse(38, 6, 260, 240)
$glowBrush = New-Object System.Drawing.Drawing2D.PathGradientBrush $glow
$glowBrush.CenterColor = [System.Drawing.Color]::FromArgb(150, 255, 190, 80)
$glowBrush.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 255, 120, 40))
$g.SetClip($inner)
$g.FillPath($glowBrush, $glow)
$g.ResetClip()

# The Extreme Demon face from GD's sprite sheet.
$sheet = [System.Drawing.Image]::FromFile((Join-Path $Resources "GJ_GameSheet03-uhd.png"))
# The sprite has the word "DEMON" baked in below row 133; keep only the face.
$face = New-Object System.Drawing.Rectangle 1164, 2338, 160, 134
$faceScale = 1.38
$fw = 160 * $faceScale; $fh = 134 * $faceScale
$g.DrawImage($sheet, (New-Object System.Drawing.RectangleF (($size - $fw) / 2), 34, $fw, $fh), $face, 'Pixel')

# "IDL" in GD's bigFont (Pusab), glyph by glyph from its atlas.
$font = [System.Drawing.Image]::FromFile((Join-Path $Resources "bigFont-uhd.png"))
$glyphs = @{}
foreach ($line in Get-Content (Join-Path $Resources "bigFont-uhd.fnt")) {
    if ($line -match '^char id=(\d+)\s+x=(\d+)\s+y=(\d+)\s+width=(\d+)\s+height=(\d+)\s+xoffset=(-?\d+)\s+yoffset=(-?\d+)\s+xadvance=(\d+)') {
        $glyphs[[int]$matches[1]] = @{ x = [int]$matches[2]; y = [int]$matches[3]; w = [int]$matches[4]; h = [int]$matches[5]; xo = [int]$matches[6]; adv = [int]$matches[8] }
    }
}
$text = "IDL"
$textScale = 0.85
$total = 0
for ($i = 0; $i -lt $text.Length; $i++) {
    $c = $glyphs[[int][char]$text[$i]]
    $total += if ($i -eq $text.Length - 1) { $c.w } else { $c.adv }
}
$x = ($size - $total * $textScale) / 2
$top = 224
foreach ($ch in $text.ToCharArray()) {
    $c = $glyphs[[int]$ch]
    $dest = New-Object System.Drawing.RectangleF ($x + $c.xo * $textScale), $top, ($c.w * $textScale), ($c.h * $textScale)
    $g.DrawImage($font, $dest, (New-Object System.Drawing.Rectangle $c.x, $c.y, $c.w, $c.h), 'Pixel')
    $x += $c.adv * $textScale
}

$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose(); $sheet.Dispose(); $font.Dispose()
"saved $Out"

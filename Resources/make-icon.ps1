# Draws the app icon (a little orange stomp box) to Resources/icon.png.
# Run once: powershell -ExecutionPolicy Bypass -File Resources\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$size = 512
$bmp = New-Object System.Drawing.Bitmap $size, $size
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)

function RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2 * $r, 2 * $r, 180, 90)
    $p.AddArc($x + $w - 2 * $r, $y, 2 * $r, 2 * $r, 270, 90)
    $p.AddArc($x + $w - 2 * $r, $y + $h - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $p.AddArc($x, $y + $h - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $p.CloseFigure()
    return $p
}
function Circle([float]$cx, [float]$cy, [float]$r) { return New-Object System.Drawing.RectangleF ($cx - $r), ($cy - $r), (2 * $r), (2 * $r) }

# shadow + body
$shadow = RoundedRect 70 44 380 450 70
$g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(90, 0, 0, 0))), $shadow)
$body = RoundedRect 66 30 380 450 70
$bodyBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 30), (New-Object System.Drawing.PointF 0, 480), ([System.Drawing.Color]::FromArgb(255, 247, 142, 72)), ([System.Drawing.Color]::FromArgb(255, 178, 76, 22))
$g.FillPath($bodyBrush, $body)
$g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(120, 0, 0, 0)), 6), $body)

# two knobs with white pointers
foreach ($k in @(@(176, 140, -40), @(336, 140, 35))) {
    $cx = $k[0]; $cy = $k[1]; $a = $k[2] * [Math]::PI / 180
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(110, 0, 0, 0))), (Circle $cx ($cy + 6) 58))
    $knobBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, ($cy - 56)), (New-Object System.Drawing.PointF 0, ($cy + 56)), ([System.Drawing.Color]::FromArgb(255, 70, 70, 78)), ([System.Drawing.Color]::FromArgb(255, 14, 14, 16))
    $g.FillEllipse($knobBrush, (Circle $cx $cy 56))
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), 12
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round; $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen, [float]($cx + 14 * [Math]::Sin($a)), [float]($cy - 14 * [Math]::Cos($a)), [float]($cx + 46 * [Math]::Sin($a)), [float]($cy - 46 * [Math]::Cos($a)))
}

# red LED with glow
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(90, 255, 60, 40))), (Circle 256 262 26))
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 59, 48))), (Circle 256 262 15))

# footswitch
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(110, 0, 0, 0))), (Circle 256 386 70))
$nutBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 190, 316), (New-Object System.Drawing.PointF 322, 448), ([System.Drawing.Color]::FromArgb(255, 235, 235, 240)), ([System.Drawing.Color]::FromArgb(255, 100, 100, 108))
$g.FillEllipse($nutBrush, (Circle 256 380 66))
$capBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 216, 340), (New-Object System.Drawing.PointF 296, 420), ([System.Drawing.Color]::FromArgb(255, 250, 250, 252)), ([System.Drawing.Color]::FromArgb(255, 130, 130, 138))
$g.FillEllipse($capBrush, (Circle 256 380 42))

$out = Join-Path $PSScriptRoot "icon.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"wrote $out"

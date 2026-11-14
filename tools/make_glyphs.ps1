# Draws the replacement button-glyph atlases (our own art) used when a PlayStation pad, an Xbox
# Series pad or the keyboard is the active input device. Layout matches the game's atlas (4 columns
# x 6 rows; see recomp/runtime/glyphs.cpp), at twice its resolution: 64-pixel cells, 256x384.
#   row 0: A B X Y            row 3: Back, Start, D-pad, Guide
#   row 1: LT RT LB RB        row 4: D-pad right, left, up, down
#   row 2: L, R, L3, R3       row 5: L left/right, L up/down, (game emblem, kept), (empty)
# Output: <OutDir>\<set>.rgba ("NGLY", width, height, RGBA8 rows) and a .png preview of each.
param([string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'recomp\build\glyphs'))
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null
$S = 64; $W = 4 * $S; $H = 6 * $S
$ink = [System.Drawing.Color]::FromArgb(255, 26, 26, 30)
$paper = [System.Drawing.Color]::FromArgb(255, 245, 245, 240)

function Brush($c) { New-Object System.Drawing.SolidBrush $c }
function Pen($c, $w) { $p = New-Object System.Drawing.Pen $c, $w; $p.LineJoin = 'Round'; $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p }
function Rgb($r, $g, $b) { [System.Drawing.Color]::FromArgb(255, $r, $g, $b) }
function RoundRect($x, $y, $w, $h, $r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure(); $p
}
function Text($g, $s, $cx, $cy, $size, $color, $font = 'Segoe UI', $style = 'Bold') {
    $f = New-Object System.Drawing.Font $font, $size, ([System.Drawing.FontStyle]$style), ([System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat; $fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
    $g.DrawString($s, $f, (Brush $color), [single]$cx, [single]($cy + $size * 0.04), $fmt)
}
# Cell centre for column c, row r.
function Cx($c) { $c * $S + $S / 2 }
function Cy($r) { $r * $S + $S / 2 }

function Disc($g, $c, $r, $fill, $radius = 26) {
    $x = (Cx $c) - $radius; $y = (Cy $r) - $radius
    $g.FillEllipse((Brush $fill), $x, $y, 2 * $radius, 2 * $radius)
    $g.DrawEllipse((Pen $ink 3), $x, $y, 2 * $radius, 2 * $radius)
}
function Pill($g, $c, $r, $fill, $w = 56, $h = 34, $rad = 12) {
    $p = RoundRect ((Cx $c) - $w / 2) ((Cy $r) - $h / 2) $w $h $rad
    $g.FillPath((Brush $fill), $p); $g.DrawPath((Pen $ink 3), $p)
}
function Key($g, $c, $r, $label, $size = 30, $w = 50, $font = 'Segoe UI') {
    $p = RoundRect ((Cx $c) - $w / 2) ((Cy $r) - 25) $w 50 9
    $g.FillPath((Brush $paper), $p); $g.DrawPath((Pen $ink 3), $p)
    $g.DrawLine((Pen (Rgb 170 170 165) 2), (Cx $c) - $w / 2 + 8, (Cy $r) + 19, (Cx $c) + $w / 2 - 8, (Cy $r) + 19)
    Text $g $label (Cx $c) ((Cy $r) - 3) $size $ink $font
}
function MiniKey($g, $x, $y, $label, $lit = $false) {
    $p = RoundRect $x $y 19 19 4
    $fill = $paper; if ($lit) { $fill = Rgb 255 208 80 }
    $g.FillPath((Brush $fill), $p); $g.DrawPath((Pen $ink 2), $p)
    Text $g $label ($x + 9.5) ($y + 9) 13 $ink
}
function Triangle($g, $cx, $cy, $size, $dir, $color) {
    $pts = switch ($dir) {
        'up'    { @((New-Object System.Drawing.PointF $cx, ($cy - $size)), (New-Object System.Drawing.PointF ($cx - $size), ($cy + $size * 0.7)), (New-Object System.Drawing.PointF ($cx + $size), ($cy + $size * 0.7))) }
        'down'  { @((New-Object System.Drawing.PointF $cx, ($cy + $size)), (New-Object System.Drawing.PointF ($cx - $size), ($cy - $size * 0.7)), (New-Object System.Drawing.PointF ($cx + $size), ($cy - $size * 0.7))) }
        'left'  { @((New-Object System.Drawing.PointF ($cx - $size), $cy), (New-Object System.Drawing.PointF ($cx + $size * 0.7), ($cy - $size)), (New-Object System.Drawing.PointF ($cx + $size * 0.7), ($cy + $size))) }
        'right' { @((New-Object System.Drawing.PointF ($cx + $size), $cy), (New-Object System.Drawing.PointF ($cx - $size * 0.7), ($cy - $size)), (New-Object System.Drawing.PointF ($cx - $size * 0.7), ($cy + $size))) }
    }
    $g.FillPolygon((Brush $color), [System.Drawing.PointF[]]$pts)
}
# A d-pad cross centred in the cell; $lit names the arm drawn highlighted ('' for none).
function Dpad($g, $c, $r, $lit, $body, $glow) {
    $cx = Cx $c; $cy = Cy $r; $a = 9; $l = 25
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($cx - $a), ($cy - $l)), (New-Object System.Drawing.PointF ($cx + $a), ($cy - $l)), (New-Object System.Drawing.PointF ($cx + $a), ($cy - $a)),
        (New-Object System.Drawing.PointF ($cx + $l), ($cy - $a)), (New-Object System.Drawing.PointF ($cx + $l), ($cy + $a)), (New-Object System.Drawing.PointF ($cx + $a), ($cy + $a)),
        (New-Object System.Drawing.PointF ($cx + $a), ($cy + $l)), (New-Object System.Drawing.PointF ($cx - $a), ($cy + $l)), (New-Object System.Drawing.PointF ($cx - $a), ($cy + $a)),
        (New-Object System.Drawing.PointF ($cx - $l), ($cy + $a)), (New-Object System.Drawing.PointF ($cx - $l), ($cy - $a)), (New-Object System.Drawing.PointF ($cx - $a), ($cy - $a))))
    $g.FillPath((Brush $body), $p)
    $arm = switch ($lit) { 'up' { @(($cx - $a), ($cy - $l), (2 * $a), ($l - $a)) } 'down' { @(($cx - $a), ($cy + $a), (2 * $a), ($l - $a)) } 'left' { @(($cx - $l), ($cy - $a), ($l - $a), (2 * $a)) } 'right' { @(($cx + $a), ($cy - $a), ($l - $a), (2 * $a)) } default { $null } }
    if ($arm) { $g.FillRectangle((Brush $glow), [single]$arm[0], [single]$arm[1], [single]$arm[2], [single]$arm[3]) }
    $g.DrawPath((Pen $ink 3), $p)
}
function Stick($g, $c, $r, $label, $arrows, $click) {
    Disc $g $c $r $paper 24
    $g.DrawEllipse((Pen (Rgb 150 150 145) 2), (Cx $c) - 17, (Cy $r) - 17, 34, 34)
    $size = 24; if ($label.Length -gt 1) { $size = 19 }
    Text $g $label (Cx $c) (Cy $r) $size $ink
    if ($arrows -eq 'h') { Triangle $g ((Cx $c) - 27) (Cy $r) 5 'left' $ink; Triangle $g ((Cx $c) + 27) (Cy $r) 5 'right' $ink }
    if ($arrows -eq 'v') { Triangle $g (Cx $c) ((Cy $r) - 27) 5 'up' $ink; Triangle $g (Cx $c) ((Cy $r) + 27) 5 'down' $ink }
    if ($click) { Triangle $g (Cx $c) ((Cy $r) - 27) 6 'down' (Rgb 255 208 80); Triangle $g (Cx $c) ((Cy $r) - 27) 4 'down' $ink }
}
function Lines($g, $c, $r) { foreach ($dy in -6, 0, 6) { $g.DrawLine((Pen $ink 3), (Cx $c) - 9, (Cy $r) + $dy, (Cx $c) + 9, (Cy $r) + $dy) } }

function DrawPad($g, $set) {
    $dark = Rgb 34 34 38
    if ($set -eq 'xbox') {
        $letters = @(@('A', (Rgb 120 200 80)), @('B', (Rgb 230 80 70)), @('X', (Rgb 80 150 235)), @('Y', (Rgb 245 200 50)))
        for ($i = 0; $i -lt 4; $i++) { Disc $g $i 0 $dark; Text $g $letters[$i][0] (Cx $i) (Cy 0) 34 $letters[$i][1] }
        $names = 'LT', 'RT', 'LB', 'RB'
    } else {
        # Cross, Circle, Square, Triangle in the A, B, X, Y positions.
        $colors = if ($set -eq 'ps4') { @((Rgb 125 178 232), (Rgb 240 110 110), (Rgb 225 150 205), (Rgb 90 210 170)) } else { @($ink, $ink, $ink, $ink) }
        $face = if ($set -eq 'ps4') { $dark } else { $paper }
        for ($i = 0; $i -lt 4; $i++) { Disc $g $i 0 $face }
        $pen = { param($c) Pen $c 5 }
        $cx = Cx 0; $cy = Cy 0; $g.DrawLine((& $pen $colors[0]), $cx - 11, $cy - 11, $cx + 11, $cy + 11); $g.DrawLine((& $pen $colors[0]), $cx + 11, $cy - 11, $cx - 11, $cy + 11)
        $cx = Cx 1; $g.DrawEllipse((& $pen $colors[1]), $cx - 13, $cy - 13, 26, 26)
        $cx = Cx 2; $g.DrawRectangle((& $pen $colors[2]), $cx - 12, $cy - 12, 24, 24)
        $cx = Cx 3; $g.DrawPolygon((& $pen $colors[3]), [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF $cx, ($cy - 15)), (New-Object System.Drawing.PointF ($cx - 15), ($cy + 11)), (New-Object System.Drawing.PointF ($cx + 15), ($cy + 11))))
        $names = 'L2', 'R2', 'L1', 'R1'
    }
    # Triggers (tall) and bumpers (flat).
    for ($i = 0; $i -lt 2; $i++) { Pill $g $i 1 $paper 46 50 12; Text $g $names[$i] (Cx $i) (Cy 1) 26 $ink }
    for ($i = 2; $i -lt 4; $i++) { Pill $g $i 1 $paper 58 32 13; Text $g $names[$i] (Cx $i) (Cy 1) 24 $ink }
    $l3 = 'L'; $r3 = 'R'; if ($set -ne 'xbox') { $l3 = 'L3'; $r3 = 'R3' }
    Stick $g 0 2 'L' '' $false; Stick $g 1 2 'R' '' $false; Stick $g 2 2 $l3 '' $true; Stick $g 3 2 $r3 '' $true
    # Back and Start.
    if ($set -eq 'xbox') {
        Disc $g 0 3 $paper 20; $g.DrawRectangle((Pen $ink 3), (Cx 0) - 9, (Cy 3) - 8, 12, 10); $g.FillRectangle((Brush $paper), (Cx 0) - 3, (Cy 3) - 2, 12, 10); $g.DrawRectangle((Pen $ink 3), (Cx 0) - 3, (Cy 3) - 2, 12, 10)
        Disc $g 1 3 $paper 20; Lines $g 1 3
    } elseif ($set -eq 'ps4') {
        Pill $g 0 3 $paper 60 26 11; Text $g 'SHARE' (Cx 0) (Cy 3) 14 $ink
        Pill $g 1 3 $paper 60 26 11; Text $g 'OPTIONS' (Cx 1) (Cy 3) 12 $ink
    } else {
        Pill $g 0 3 $paper 30 44 12; foreach ($a in -35, 0, 35) { $rad = ($a - 90) * [Math]::PI / 180; $g.DrawLine((Pen $ink 3), (Cx 0) + 5 * [Math]::Cos($rad), (Cy 3) + 4 + 5 * [Math]::Sin($rad), (Cx 0) + 14 * [Math]::Cos($rad), (Cy 3) + 4 + 14 * [Math]::Sin($rad)) }
        Pill $g 1 3 $paper 30 44 12; Lines $g 1 3
    }
    $body = Rgb 70 70 76; if ($set -eq 'ps5') { $body = $paper }
    Dpad $g 2 3 '' $body $body
    Disc $g 3 3 $dark 24
    if ($set -eq 'xbox') { $g.DrawLine((Pen $paper 5), (Cx 3) - 10, (Cy 3) - 10, (Cx 3) + 10, (Cy 3) + 10); $g.DrawLine((Pen $paper 5), (Cx 3) + 10, (Cy 3) - 10, (Cx 3) - 10, (Cy 3) + 10) } else { Text $g 'PS' (Cx 3) (Cy 3) 24 $paper }
    $glow = Rgb 255 208 80
    $i = 0; foreach ($dir in 'right', 'left', 'up', 'down') { Dpad $g $i 4 $dir $body $glow; $i++ }
    Stick $g 0 5 'L' 'h' $false; Stick $g 1 5 'L' 'v' $false
}

function DrawKeyboard($g) {
    $sym = 'Segoe UI Symbol'
    Key $g 0 0 ([string][char]0x2423) 30 58 $sym      # A: Space
    Key $g 1 0 ([string][char]0x21E7) 30 50 $sym      # B: Left Shift
    Key $g 2 0 'E'; Key $g 3 0 'R'                     # X, Y
    Key $g 0 1 'Z'; Key $g 1 1 'C'; Key $g 2 1 'Q'; Key $g 3 1 'F'   # LT RT LB RB
    $cluster = { param($c, $r, $keys, $lit)   # up, left, down, right
        $x = (Cx $c) - 30; $y = (Cy $r) - 20
        MiniKey $g ($x + 20.5) $y $keys[0] ($lit -contains 0); MiniKey $g $x ($y + 21) $keys[1] ($lit -contains 1)
        MiniKey $g ($x + 20.5) ($y + 21) $keys[2] ($lit -contains 2); MiniKey $g ($x + 41) ($y + 21) $keys[3] ($lit -contains 3) }
    & $cluster 0 2 @('W', 'A', 'S', 'D') @()            # L stick
    & $cluster 1 2 @('I', 'J', 'K', 'L') @()            # R stick
    Key $g 2 2 'V'; Key $g 3 2 'B'                      # L3, R3
    Key $g 0 3 'Tab' 21 58; Key $g 1 3 ([string][char]0x21B5) 30 58 $sym   # Back, Start (Enter)
    $arrows = @([string][char]0x2191, [string][char]0x2190, [string][char]0x2193, [string][char]0x2192)
    & $cluster 2 3 $arrows @()                          # D-pad: arrow keys
    Key $g 3 3 'Esc' 21 58                              # Guide
    Key $g 0 4 $arrows[3] 30 50 $sym; Key $g 1 4 $arrows[1] 30 50 $sym; Key $g 2 4 $arrows[0] 30 50 $sym; Key $g 3 4 $arrows[2] 30 50 $sym
    & $cluster 0 5 @('W', 'A', 'S', 'D') @(1, 3)        # L left/right
    & $cluster 1 5 @('W', 'A', 'S', 'D') @(0, 2)        # L up/down
}

foreach ($set in 'xbox', 'ps4', 'ps5', 'keyboard') {
    $bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAliasGridFit'; $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::FromArgb(0, 26, 26, 30))
    if ($set -eq 'keyboard') { DrawKeyboard $g } else { DrawPad $g $set }
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir "$set.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $W, $H
    $data = $bmp.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($W * $H * 4)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data); $bmp.Dispose()
    for ($i = 0; $i -lt $bytes.Length; $i += 4) { $b = $bytes[$i]; $bytes[$i] = $bytes[$i + 2]; $bytes[$i + 2] = $b }   # BGRA -> RGBA
    $out = [System.IO.File]::Create((Join-Path $OutDir "$set.rgba"))
    $out.Write([System.Text.Encoding]::ASCII.GetBytes('NGLY'), 0, 4); $out.Write([BitConverter]::GetBytes([uint32]$W), 0, 4); $out.Write([BitConverter]::GetBytes([uint32]$H), 0, 4)
    $out.Write($bytes, 0, $bytes.Length); $out.Close()
    "$set glyphs written"
}

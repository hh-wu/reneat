# recolor-icons.ps1 — shift green hues to red in extracted NeatDM icons,
# repack as PNG-compressed .ico files (Vista+). Originals backed up to res/extracted/orig/.
Add-Type -AssemblyName System.Drawing

$src = 'C:\Users\user\xdm\reneat\res\extracted'
$bak = "$src\orig"
New-Item -ItemType Directory -Force $bak | Out-Null

function Recolor-Bitmap([System.Drawing.Bitmap]$bmp) {
    $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadWrite,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $len = $data.Stride * $data.Height
    $px = New-Object byte[] $len
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $len)
    for ($i = 0; $i -lt $len; $i += 4) {
        $b = $px[$i]; $g = $px[$i+1]; $r = $px[$i+2]; $a = $px[$i+3]
        if ($a -eq 0) { continue }
        $max = [Math]::Max($r, [Math]::Max($g, $b))
        $min = [Math]::Min($r, [Math]::Min($g, $b))
        $delta = $max - $min
        if ($delta -lt 25) { continue }               # gray/white/black: keep
        # hue of the pixel (degrees 0-360)
        if ($max -eq $g)      { $h = 60 * (($b - $r) / $delta) + 120 }
        elseif ($max -eq $b)  { $h = 60 * (($r - $g) / $delta) + 240 }
        else                  { $h = 60 * (($g - $b) / $delta); if ($h -lt 0) { $h += 360 } }
        if ($h -lt 60 -or $h -gt 175) { continue }    # not green: keep
        # rotate hue: green(120) -> red(0)
        $h -= 120; if ($h -lt 0) { $h += 360 }
        $s = $delta / [double]$max
        $v = $max / 255.0
        # HSV -> RGB
        $c = $v * $s
        $hp = $h / 60.0
        $x = $c * (1 - [Math]::Abs(($hp % 2) - 1))
        $m = $v - $c
        $r1=0.0;$g1=0.0;$b1=0.0
        if     ($hp -lt 1) { $r1=$c; $g1=$x }
        elseif ($hp -lt 2) { $r1=$x; $g1=$c }
        elseif ($hp -lt 3) { $g1=$c; $b1=$x }
        elseif ($hp -lt 4) { $g1=$x; $b1=$c }
        elseif ($hp -lt 5) { $b1=$c; $r1=$x }
        else               { $b1=$x; $r1=$c }
        $px[$i]   = [byte]([Math]::Round(($b1 + $m) * 255))
        $px[$i+1] = [byte]([Math]::Round(($g1 + $m) * 255))
        $px[$i+2] = [byte]([Math]::Round(($r1 + $m) * 255))
    }
    [System.Runtime.InteropServices.Marshal]::Copy($px, 0, $data.Scan0, $len)
    $bmp.UnlockBits($data)
}

function Write-Ico([byte[][]]$pngFrames, [int[]]$sizes, [string]$outPath) {
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter($ms)
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
    $off = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $sz = $sizes[$i]
        $w.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
        $w.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
        $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32)
        $w.Write([uint32]$pngFrames[$i].Length); $w.Write([uint32]$off)
        $off += $pngFrames[$i].Length
    }
    foreach ($f in $pngFrames) { $w.Write($f) }
    [System.IO.File]::WriteAllBytes($outPath, $ms.ToArray())
}

function Recolor-Icon([string]$icoPath, [int[]]$sizes) {
    $name = Split-Path $icoPath -Leaf
    # keep a pristine GREEN original in orig/ forever; never overwrite it
    # (PNG-compressed ICOs can't be re-read by GDI+, so re-running on output breaks)
    if (-not (Test-Path "$bak\$name")) { Copy-Item $icoPath "$bak\$name" }
    $ico = New-Object System.Drawing.Icon("$bak\$name", 4096, 4096)
    $bmp = $ico.ToBitmap()
    $big = New-Object System.Drawing.Bitmap $bmp, $bmp.Width, $bmp.Height
    Recolor-Bitmap $big
    $frames = @()
    foreach ($sz in $sizes) {
        $rs = New-Object System.Drawing.Bitmap $sz, $sz
        $g = [System.Drawing.Graphics]::FromImage($rs)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($big, 0, 0, $sz, $sz)
        $g.Dispose()
        $pngMs = New-Object System.IO.MemoryStream
        $rs.Save($pngMs, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames += ,$pngMs.ToArray()
        $rs.Dispose()
    }
    Write-Ico $frames $sizes $icoPath
    "recolored $name -> red ($($sizes -join ','))  $($frames[0].Length)b"
    # also export a plain PNG master for other consumers (extension icons etc.)
    if ($name -eq 'icon_103.ico') { $big.Save("$src\logo_red.png", [System.Drawing.Imaging.ImageFormat]::Png) }
    $big.Dispose(); $bmp.Dispose(); $ico.Dispose()
}

Recolor-Icon "$src\icon_101.ico" @(32)
Recolor-Icon "$src\icon_102.ico" @(16)
Recolor-Icon "$src\icon_103.ico" @(64, 48, 32, 16)
Recolor-Icon "$src\icon_119.ico" @(16)
Recolor-Icon "$src\icon_120.ico" @(16)

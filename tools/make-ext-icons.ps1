# make-ext-icons.ps1 — regenerate extension PNG icons from the recolored logo PNG
# (res/extracted/logo_red.png is produced by tools/recolor-icons.ps1)
Add-Type -AssemblyName System.Drawing

$srcPng = 'C:\Users\user\xdm\reneat\res\extracted\logo_red.png'
$outDir = 'C:\Users\user\xdm\reneat\extension\icons'
New-Item -ItemType Directory -Force $outDir | Out-Null

$big = [System.Drawing.Bitmap]::FromFile($srcPng)
foreach ($sz in 16, 48, 128) {
    $rs = New-Object System.Drawing.Bitmap $sz, $sz
    $g = [System.Drawing.Graphics]::FromImage($rs)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($big, 0, 0, $sz, $sz)
    $g.Dispose()
    $rs.Save("$outDir\icon$sz.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $rs.Dispose()
    $len = (Get-Item "$outDir\icon$sz.png").Length
    "icon$sz.png regenerated ($len bytes)"
}
$big.Dispose()

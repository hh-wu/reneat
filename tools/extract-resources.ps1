# Extract resources (icons, bitmaps, PNGs, manifests) from a PE file's .rsrc section.
# Usage: extract-resources.ps1 <exe> <outdir>
param(
  [string]$Exe = 'C:\Users\user\.dsh\attachments\v1\files\60\60b06db7dfeb6fffb1be82f8ad059d61bdb1b1a3889439b56eaac162e64c0f37\NeatDM.exe',
  [string]$OutDir = 'C:\Users\user\xdm\reneat\res\extracted'
)

$bytes = [System.IO.File]::ReadAllBytes($Exe)
$peoff = [BitConverter]::ToInt32($bytes, 0x3c)
$optBase = $peoff + 24
$magic = [BitConverter]::ToUInt16($bytes, $optBase)
if ($magic -eq 0x10b) { $ddBase = $optBase + 96 } else { $ddBase = $optBase + 112 }
$rsrcRva  = [BitConverter]::ToUInt32($bytes, $ddBase + 8 * 2)
$rsrcSize = [BitConverter]::ToUInt32($bytes, $ddBase + 8 * 2 + 4)

# section table
$nsec = [BitConverter]::ToUInt16($bytes, $peoff + 6)
$optSize = [BitConverter]::ToUInt16($bytes, $peoff + 20)
$secBase = $peoff + 24 + $optSize
function R2O([uint32]$rva) {
  for ($i = 0; $i -lt $nsec; $i++) {
    $o = $secBase + 40 * $i
    $vaddr = [BitConverter]::ToUInt32($bytes, $o + 12)
    $rsize = [BitConverter]::ToUInt32($bytes, $o + 16)
    $roff  = [BitConverter]::ToUInt32($bytes, $o + 20)
    $vsize = [BitConverter]::ToUInt32($bytes, $o + 8)
    $span = [Math]::Max($vsize, $rsize)
    if ($rva -ge $vaddr -and $rva -lt ($vaddr + $span)) { return $rva - $vaddr + $roff }
  }
  return 0
}

$rsrcOff = R2O $rsrcRva
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Read-Name([uint32]$entryOff, [bool]$isDir) {
  $nameField = [BitConverter]::ToUInt32($bytes, $entryOff)
  if ($nameField -band 0x80000000) {
    $no = $rsrcOff + ($nameField -band 0x7fffffff)
    $len = [BitConverter]::ToUInt16($bytes, $no)
    return [System.Text.Encoding]::Unicode.GetString($bytes, $no + 2, $len * 2)
  } else {
    return "#$nameField"
  }
}

$results = @()
function Walk([uint32]$dirRel, [string]$path) {
  $d = $rsrcOff + $dirRel
  $nNamed = [BitConverter]::ToUInt16($bytes, $d + 12)
  $nId    = [BitConverter]::ToUInt16($bytes, $d + 14)
  $total = $nNamed + $nId
  for ($i = 0; $i -lt $total; $i++) {
    $e = $d + 16 + 8 * $i
    $name = Read-Name $e $true
    $data = [BitConverter]::ToUInt32($bytes, $e + 4)
    $newPath = if ($path) { "$path/$name" } else { $name }
    if ($data -band 0x80000000) {
      Walk ($data -band 0x7fffffff) $newPath
    } else {
      $de = $rsrcOff + $data
      $dataRva = [BitConverter]::ToUInt32($bytes, $de)
      $size    = [BitConverter]::ToUInt32($bytes, $de + 4)
      $codepg  = [BitConverter]::ToUInt32($bytes, $de + 8)
      $fo = R2O $dataRva
      $script:results += [pscustomobject]@{ Path = $newPath; Off = $fo; Size = $size; CodePage = $codepg }
    }
  }
}
Walk 0 ''

$typeNames = @{1='CURSOR';2='BITMAP';3='ICON';4='MENU';5='DIALOG';6='STRING';9='ACCELERATOR';10='RCDATA';11='MESSAGETABLE';12='GROUP_CURSOR';14='GROUP_ICON';16='VERSION';24='MANIFEST'}
$iconGroups = @{}   # groupId -> list of entries
$iconImages = @{}   # iconId -> bytes

foreach ($r in $results) {
  $parts = $r.Path -split '/'
  $typeId = [int]($parts[0].TrimStart('#'))
  $typeName = $typeNames[$typeId]; if (-not $typeName) { $typeName = "TYPE$typeId" }
  $resId = $parts[1]
  $raw = New-Object byte[] $r.Size
  [Array]::Copy($bytes, $r.Off, $raw, 0, $r.Size)

  switch ($typeId) {
    3 { # ICON single image (ICO data without header)
        $iconImages[$resId] = $raw; break }
    14 { $iconGroups[$resId] = $raw; break }
    2 { # BITMAP -> prepend BMP file header
        $hdr = New-Object byte[] 14
        $hdr[0] = 0x42; $hdr[1] = 0x4D # 'BM'
        $sz = $r.Size + 14
        [BitConverter]::GetBytes([uint32]$sz).CopyTo($hdr, 2)
        # offset to pixel data: 14 + BITMAPINFOHEADER size + palette
        $biSize = [BitConverter]::ToUInt32($raw, 0)
        $bpp = [BitConverter]::ToUInt16($raw, 14)
        $clrUsed = [BitConverter]::ToUInt32($raw, 32)
        $palette = 0
        if ($bpp -le 8) { $palette = ($(if ($clrUsed) { $clrUsed } else { 1 -shl $bpp })) * 4 }
        $pixelOff = 14 + $biSize + $palette
        [BitConverter]::GetBytes([uint32]$pixelOff).CopyTo($hdr, 10)
        $out = New-Object byte[] ($raw.Length + 14)
        $hdr.CopyTo($out, 0); $raw.CopyTo($out, 14)
        [System.IO.File]::WriteAllBytes("$OutDir\bitmap_$resId.bmp", $out)
        break }
    24 { [System.IO.File]::WriteAllBytes("$OutDir\manifest.xml", $raw); break }
    10 { [System.IO.File]::WriteAllBytes("$OutDir\rcdata_$($resId -replace '[\\/]','_').bin", $raw); break }
    16 { [System.IO.File]::WriteAllBytes("$OutDir\version.bin", $raw); break }
    6 { [System.IO.File]::WriteAllBytes("$OutDir\strings_$resId.bin", $raw); break }
    default { [System.IO.File]::WriteAllBytes("$OutDir\$($typeName)_$($resId -replace '[\\/]','_').bin", $raw) }
  }
}

# Rebuild .ico files: ICONDIR + entries from GROUP_ICON, images from ICON
foreach ($gid in $iconGroups.Keys) {
  $g = $iconGroups[$gid]
  $count = [BitConverter]::ToUInt16($g, 4)
  $ico = New-Object System.IO.MemoryStream
  $w = New-Object System.IO.BinaryWriter($ico)
  $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$count)
  $imgOff = 6 + 16 * $count
  for ($i = 0; $i -lt $count; $i++) {
    $e = 6 + 14 * $i
    $imgId = [BitConverter]::ToUInt16($g, $e + 12)
    $img = $iconImages["#$imgId"]
    $w.Write($g[$e]); $w.Write($g[$e+1]); $w.Write($g[$e+2]); $w.Write($g[$e+3])
    $w.Write([BitConverter]::ToUInt16($g, $e + 4)); $w.Write([BitConverter]::ToUInt16($g, $e + 6))
    $w.Write([uint32]$img.Length); $w.Write([uint32]$imgOff)
    $imgOff += $img.Length
  }
  for ($i = 0; $i -lt $count; $i++) {
    $e = 6 + 14 * $i
    $imgId = [BitConverter]::ToUInt16($g, $e + 12)
    $w.Write($iconImages["#$imgId"])
  }
  [System.IO.File]::WriteAllBytes("$OutDir\icon_$($gid.TrimStart('#')).ico", $ico.ToArray())
}

Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize
"Resource entries: $($results.Count)"

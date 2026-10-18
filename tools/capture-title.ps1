# capture-title.ps1 — capture a top-level window by title without requiring
# cross-process handle access (useful in restricted build sandboxes).
param([string]$Title = 'reneat', [string]$Out = 'shot.png')
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WinCapTitle {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    public struct RECT { public int left, top, right, bottom; }
    public static List<IntPtr> Find(string needle) {
        var hits = new List<IntPtr>();
        EnumWindows((h, l) => {
            if (IsWindowVisible(h)) {
                var sb = new StringBuilder(512);
                GetWindowTextW(h, sb, sb.Capacity);
                if (sb.ToString().IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0) hits.Add(h);
            }
            return true;
        }, IntPtr.Zero);
        return hits;
    }
}
'@
$hits = [WinCapTitle]::Find($Title)
if ($hits.Count -eq 0) { Write-Error "no visible window titled like '$Title'"; exit 1 }
$best = $null; $bestArea = -1; $rect = New-Object WinCapTitle+RECT
foreach ($h in $hits) {
    $r = New-Object WinCapTitle+RECT
    [WinCapTitle]::GetWindowRect($h, [ref]$r) | Out-Null
    $area = ($r.right-$r.left) * ($r.bottom-$r.top)
    if ($area -gt $bestArea) { $best = $h; $bestArea = $area; $rect = $r }
}
$w = $rect.right-$rect.left; $h = $rect.bottom-$rect.top
if ($w -le 0 -or $h -le 0) { Write-Error "bad window rect ${w}x${h}"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w,$h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[WinCapTitle]::PrintWindow($best, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $Out ${w}x${h} ($($hits.Count) matching)"

# capture-window.ps1 — capture window of a process by title substring (thread-enum, cross-desktop)
param([string]$ProcName = 'reneat', [string]$Title = '', [string]$Out = 'shot.png')
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WinCap3 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumThreadWindows(uint tid, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public struct RECT { public int left, top, right, bottom; }
    public static List<IntPtr> OfProcess(int pid, string needle) {
        var hits = new List<IntPtr>();
        var p = System.Diagnostics.Process.GetProcessById(pid);
        foreach (System.Diagnostics.ProcessThread t in p.Threads) {
            EnumThreadWindows((uint)t.Id, (h, l) => {
                var sb = new StringBuilder(512);
                GetWindowTextW(h, sb, 512);
                if (needle == "" || sb.ToString().IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0)
                    hits.Add(h);
                return true;
            }, IntPtr.Zero);
        }
        return hits;
    }
}
'@
$p = Get-Process $ProcName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Error "no process $ProcName"; exit 1 }
$hits = [WinCap3]::OfProcess($p.Id, $Title)
if ($hits.Count -eq 0) { Write-Error "no window titled like '$Title'"; exit 1 }
$best = $null; $bestArea = -1
foreach ($h in $hits) {
    $r = [WinCap3+RECT]::new()
    [WinCap3]::GetWindowRect($h, [ref]$r) | Out-Null
    $a = ($r.right-$r.left) * ($r.bottom-$r.top)
    if ($a -gt $bestArea) { $bestArea = $a; $best = $h }
}
$r = [WinCap3+RECT]::new()
[WinCap3]::GetWindowRect($best, [ref]$r) | Out-Null
$w = $r.right - $r.left; $hgt = $r.bottom - $r.top
if ($w -le 0 -or $hgt -le 0) { Write-Error "bad rect ${w}x${hgt}"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[WinCap3]::PrintWindow($best, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ${w}x${hgt} (of $($hits.Count) matching)"

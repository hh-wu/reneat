# capture-big.ps1 — capture the biggest (or index-th) visible window of a process
param([string]$ProcName='reneat', [string]$Out='shot.png', [int]$Index=0)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Collections.Generic;using System.Runtime.InteropServices;
public class WC9 { public delegate bool P(IntPtr h,IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumThreadWindows(uint tid,P cb,IntPtr l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 public struct RECT{public int left,top,right,bottom;}
 public static List<IntPtr> Handles(int pid){ var r=new List<IntPtr>(); var p=System.Diagnostics.Process.GetProcessById(pid);
  foreach(System.Diagnostics.ProcessThread t in p.Threads){ EnumThreadWindows((uint)t.Id,(h,l)=>{ if(IsWindowVisible(h)) r.Add(h); return true;},IntPtr.Zero);} return r; }
 public static RECT Rect(IntPtr h){ RECT r; GetWindowRect(h,out r); return r; } }
'@
$p = Get-Process $ProcName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Error "no process $ProcName"; exit 1 }
$hs = [WC9]::Handles($p.Id)
if ($hs.Count -eq 0) { Write-Error "no visible windows"; exit 1 }
$sorted = $hs | Sort-Object { $r=[WC9]::Rect($_); -($r.right-$r.left)*($r.bottom-$r.top) }
$pick = $sorted[[Math]::Min($Index, $sorted.Count-1)]
$r = [WC9]::Rect($pick)
$w = $r.right-$r.left; $hh = $r.bottom-$r.top
$bmp = New-Object Drawing.Bitmap $w,$hh
$g = [Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[WC9]::PrintWindow($pick, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$bmp.Save($Out)
"saved $Out ${w}x${hh} ($($hs.Count) windows, picked #$Index)"

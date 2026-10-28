param([string]$exe,[string]$dir,[string]$shot)
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class P{[DllImport("user32.dll")]public static extern IntPtr FindWindow(string c,string t);
[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);
[DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
[DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
public struct RECT{public int L,T,R,B;}}
"@
Remove-Item Env:GHOSTPC_NOSETUP -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru
Start-Sleep -Milliseconds 4500
$h=[P]::FindWindow("GhostPCSettings",$null)
if($h -ne [IntPtr]::Zero){ $r=New-Object P+RECT; [P]::GetWindowRect($h,[ref]$r)|Out-Null
 $bmp=New-Object System.Drawing.Bitmap ($r.R-$r.L),($r.B-$r.T); $g=[System.Drawing.Graphics]::FromImage($bmp); $dc=$g.GetHdc(); [P]::PrintWindow($h,$dc,2)|Out-Null; $g.ReleaseHdc($dc); $bmp.Save($shot)
 [P]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null; "dialog captured" } else { "no dialog" }
Start-Sleep -Milliseconds 1500
if(-not $p.HasExited){ Stop-Process -Id $p.Id -Force; "killed" } else { "exited " + $p.ExitCode }

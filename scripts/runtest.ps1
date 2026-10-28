param([string]$exe,[string]$dir,[int]$secs=15,[string]$shot="",[string]$gameArgs="")
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;using System.Collections.Generic;
public class W{
 public delegate bool EP(IntPtr h,IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EP f,IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p,EP f,IntPtr l);
 [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h,out int pid);
 [DllImport("user32.dll",CharSet=CharSet.Auto)] public static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Auto)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 public static List<string> Dump(int pid){var r=new List<string>();EnumWindows((h,l)=>{int p;GetWindowThreadProcessId(h,out p);if(p==pid&&IsWindowVisible(h)){var s=new StringBuilder(512);var c=new StringBuilder(256);GetWindowText(h,s,512);GetClassName(h,c,256);r.Add("WIN ["+c+"] "+s);EnumChildWindows(h,(h2,l2)=>{var s2=new StringBuilder(2048);GetWindowText(h2,s2,2048);if(s2.Length>0)r.Add("   child: "+s2);return true;},IntPtr.Zero);}return true;},IntPtr.Zero);return r;}
}
"@
$env:GHOSTPC_NOSETUP = "1"
if($gameArgs){ $p = Start-Process -FilePath $exe -WorkingDirectory $dir -ArgumentList $gameArgs -PassThru } else { $p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru }
$t=0
while($t -lt $secs -and -not $p.HasExited){ Start-Sleep -Milliseconds 1000; $t++ }
if($shot){ Add-Type -AssemblyName System.Windows.Forms,System.Drawing; $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height; $g=[System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size); $bmp.Save($shot) }
if($p.HasExited){ "EXITED code=0x{0:X8} after {1}s" -f $p.ExitCode,$t } else { [W]::Dump($p.Id) | % { $_ }; "RUNNING after $t s, killing"; Stop-Process -Id $p.Id -Force }

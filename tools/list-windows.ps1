Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WEnum {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$procs = Get-Process SoundRadar -ErrorAction SilentlyContinue
if (-not $procs) { Write-Output "no SoundRadar process"; exit }
foreach ($pr in $procs) {
    $targetPid = $pr.Id
    Write-Output "PID $targetPid :"
    $cb = [WEnum+EnumProc]{
        param($h, $l)
        $wpid = 0
        [WEnum]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
        if ($wpid -eq $targetPid) {
            $t = New-Object Text.StringBuilder 256
            $c = New-Object Text.StringBuilder 256
            [WEnum]::GetWindowTextW($h, $t, 256) | Out-Null
            [WEnum]::GetClassNameW($h, $c, 256) | Out-Null
            $r = New-Object WEnum+RECT
            [WEnum]::GetWindowRect($h, [ref]$r) | Out-Null
            $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
            Write-Output ("  hwnd={0} visible={1} {2}x{3} @({4},{5}) title=[{6}] class=[{7}]" -f $h, ([WEnum]::IsWindowVisible($h)), $w, $hh, $r.Left, $r.Top, $t, $c)
        }
        return $true
    }
    [WEnum]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
}

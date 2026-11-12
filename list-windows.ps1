# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [string]$Title = ''
)

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WindowSizeProbe {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    public delegate bool EnumCallback(IntPtr hwnd, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCallback callback, IntPtr param);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
}
'@

$results = [System.Collections.Generic.List[object]]::new()
$callback = [WindowSizeProbe+EnumCallback] {
    param($hwnd, $unused)
    if (-not [WindowSizeProbe]::IsWindowVisible($hwnd) -or [WindowSizeProbe]::IsIconic($hwnd)) {
        return $true
    }
    $text = [System.Text.StringBuilder]::new(1024)
    [void][WindowSizeProbe]::GetWindowText($hwnd, $text, $text.Capacity)
    $name = $text.ToString()
    if (-not $name -or ($Title -and $name -notlike "*$Title*")) {
        return $true
    }
    $rect = [WindowSizeProbe+Rect]::new()
    if (-not [WindowSizeProbe]::GetClientRect($hwnd, [ref]$rect)) {
        return $true
    }
    [uint32]$processId = 0
    [void][WindowSizeProbe]::GetWindowThreadProcessId($hwnd, [ref]$processId)
    $processName = try { (Get-Process -Id $processId -ErrorAction Stop).ProcessName + '.exe' } catch { '' }
    $results.Add([pscustomobject]@{
        Title = $name
        Executable = $processName
        ClientWidth = $rect.Right - $rect.Left
        ClientHeight = $rect.Bottom - $rect.Top
        PID = $processId
        HWND = ('0x{0:X}' -f $hwnd.ToInt64())
    })
    return $true
}
[void][WindowSizeProbe]::EnumWindows($callback, [IntPtr]::Zero)
$results | Sort-Object Title, ClientWidth, ClientHeight | Format-Table -AutoSize -Wrap

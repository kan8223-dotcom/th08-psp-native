param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,
    [int]$X = 80,
    [int]$Y = 80
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PpssppVisibleWindow
{
    [DllImport("user32.dll", SetLastError = true)]
    public static extern int GetWindowLong(IntPtr window, int index);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern int SetWindowLong(IntPtr window, int index, int value);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetLayeredWindowAttributes(
        IntPtr window, uint colorKey, byte alpha, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(
        IntPtr window, IntPtr insertAfter, int x, int y,
        int width, int height, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool ShowWindowAsync(IntPtr window, int command);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetForegroundWindow(IntPtr window);
}
'@

$process = Get-Process -Id $TargetProcessId -ErrorAction Stop
if ($process.ProcessName -ne "PPSSPPWindows64") {
    throw "process $TargetProcessId is $($process.ProcessName), not PPSSPPWindows64"
}
$window = $process.MainWindowHandle
if ($window -eq [IntPtr]::Zero) {
    throw "PPSSPP process $TargetProcessId has no main window"
}

$gwlExStyle = -20
$wsExTransparent = 0x00000020
$wsExToolWindow = 0x00000080
$wsExLayered = 0x00080000
$wsExNoActivate = 0x08000000
$lwaAlpha = 0x00000002
$swRestore = 9
$swpNoSize = 0x0001
$swpShowWindow = 0x0040
$swpFrameChanged = 0x0020
$notTopmost = [IntPtr](-2)

# Restore opacity before removing WS_EX_LAYERED, then remove every style added
# by launch_ppsspp_invisible.ps1. Keep the emulator windowed and non-topmost.
[void][PpssppVisibleWindow]::SetLayeredWindowAttributes(
    $window, 0, 255, $lwaAlpha)
$style = [PpssppVisibleWindow]::GetWindowLong($window, $gwlExStyle)
$addedStyles = $wsExTransparent -bor $wsExToolWindow -bor `
    $wsExLayered -bor $wsExNoActivate
$style = $style -band (-bnot $addedStyles)
[void][PpssppVisibleWindow]::SetWindowLong($window, $gwlExStyle, $style)
[void][PpssppVisibleWindow]::ShowWindowAsync($window, $swRestore)
$positionFlags = $swpNoSize -bor $swpShowWindow -bor $swpFrameChanged
if (-not [PpssppVisibleWindow]::SetWindowPos(
        $window, $notTopmost, $X, $Y, 0, 0, $positionFlags)) {
    throw "SetWindowPos failed"
}
[void][PpssppVisibleWindow]::SetForegroundWindow($window)

[pscustomobject]@{
    ProcessId = $process.Id
    Window = ("0x{0:x}" -f $window.ToInt64())
    Alpha = 255
    X = $X
    Y = $Y
    Topmost = $false
} | ConvertTo-Json -Compress

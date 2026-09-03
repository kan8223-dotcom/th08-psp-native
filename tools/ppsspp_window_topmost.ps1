param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,
    [switch]$Disable
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PpssppWindowOrder
{
    [DllImport("user32.dll")]
    public static extern bool ShowWindowAsync(IntPtr window, int command);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter,
        int x, int y, int width, int height, uint flags);
}
'@

$process = Get-Process -Id $TargetProcessId -ErrorAction Stop
if ($process.ProcessName -ne "PPSSPPWindows64") {
    throw "process $TargetProcessId is not PPSSPPWindows64"
}

$topmost = [IntPtr](-1)
$notTopmost = [IntPtr](-2)
$noMoveOrResize = 0x0001 -bor 0x0002 -bor 0x0040
[void][PpssppWindowOrder]::ShowWindowAsync($process.MainWindowHandle, 9)
[void][PpssppWindowOrder]::SetWindowPos(
    $process.MainWindowHandle,
    $(if ($Disable) { $notTopmost } else { $topmost }),
    0, 0, 0, 0, $noMoveOrResize)
if (-not $Disable) {
    [void][PpssppWindowOrder]::SetForegroundWindow($process.MainWindowHandle)
}

param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,
    [Parameter(Mandatory = $true)]
    [int]$X,
    [Parameter(Mandatory = $true)]
    [int]$Y
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PpssppMouseInput
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint x, uint y, uint data,
                                          UIntPtr extraInfo);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);
}
'@

$process = Get-Process -Id $TargetProcessId -ErrorAction Stop
if ($process.ProcessName -ne "PPSSPPWindows64") {
    throw "process $TargetProcessId is $($process.ProcessName), not PPSSPPWindows64"
}
if ($process.MainWindowHandle -eq [IntPtr]::Zero) {
    throw "PPSSPP process $TargetProcessId has no main window"
}

$rect = New-Object PpssppMouseInput+Rect
if (-not [PpssppMouseInput]::GetWindowRect($process.MainWindowHandle, [ref]$rect)) {
    throw "GetWindowRect failed"
}

[void][PpssppMouseInput]::SetForegroundWindow($process.MainWindowHandle)
Start-Sleep -Milliseconds 100
[void][PpssppMouseInput]::SetCursorPos($rect.Left + $X, $rect.Top + $Y)
[PpssppMouseInput]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 35
[PpssppMouseInput]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)

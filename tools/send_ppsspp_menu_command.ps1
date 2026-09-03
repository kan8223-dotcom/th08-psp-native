param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,
    [Parameter(Mandatory = $true)]
    [int]$CommandId
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PpssppMenuCommand
{
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SendMessage(
        IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
}
'@

$process = Get-Process -Id $TargetProcessId -ErrorAction Stop
if ($process.ProcessName -ne "PPSSPPWindows64") {
    throw "process $TargetProcessId is $($process.ProcessName), not PPSSPPWindows64"
}
if ($process.MainWindowHandle -eq [IntPtr]::Zero) {
    throw "PPSSPP process $TargetProcessId has no main window"
}

$wmCommand = 0x0111
[void][PpssppMenuCommand]::SendMessage(
    $process.MainWindowHandle, $wmCommand, [IntPtr]$CommandId, [IntPtr]::Zero)

[pscustomobject]@{
    ProcessId = $process.Id
    CommandId = $CommandId
} | ConvertTo-Json -Compress

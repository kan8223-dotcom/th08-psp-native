param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PpssppInvisibleCapture
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PrintWindow(IntPtr window, IntPtr deviceContext, uint flags);
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

$rect = New-Object PpssppInvisibleCapture+Rect
if (-not [PpssppInvisibleCapture]::GetWindowRect($window, [ref]$rect)) {
    throw "GetWindowRect failed"
}
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) {
    throw "invalid PPSSPP window dimensions: ${width}x${height}"
}

$directory = [System.IO.Path]::GetDirectoryName(
    [System.IO.Path]::GetFullPath($OutputPath))
[System.IO.Directory]::CreateDirectory($directory) | Out-Null

$bitmap = [System.Drawing.Bitmap]::new($width, $height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$deviceContext = [IntPtr]::Zero
try {
    $deviceContext = $graphics.GetHdc()
    # PW_RENDERFULLCONTENT asks DWM to render an occluded/off-screen window.
    # It neither moves, activates nor changes the alpha of PPSSPP.
    if (-not [PpssppInvisibleCapture]::PrintWindow(
            $window, $deviceContext, 0x00000002)) {
        throw "PrintWindow failed"
    }
    $graphics.ReleaseHdc($deviceContext)
    $deviceContext = [IntPtr]::Zero
    $bitmap.Save(
        [System.IO.Path]::GetFullPath($OutputPath),
        [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    if ($deviceContext -ne [IntPtr]::Zero) {
        $graphics.ReleaseHdc($deviceContext)
    }
    $graphics.Dispose()
    $bitmap.Dispose()
}

[pscustomobject]@{
    ProcessId = $TargetProcessId
    Width = $width
    Height = $height
    OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
} | ConvertTo-Json -Compress

param(
    [Parameter(Mandatory = $true)]
    [string]$PpssppRoot,
    [Parameter(Mandatory = $true)]
    [string]$EbootPath
)

$ErrorActionPreference = "Stop"

$executable = Join-Path $PpssppRoot "PPSSPPWindows64.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "PPSSPP executable not found: $executable"
}
if (-not (Test-Path -LiteralPath $EbootPath -PathType Leaf)) {
    throw "EBOOT not found: $EbootPath"
}

$existing = @(Get-Process -Name "PPSSPPWindows64" -ErrorAction SilentlyContinue)
if ($existing.Count -ne 0) {
    throw "Refusing to launch: $($existing.Count) PPSSPP process(es) already exist"
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PpssppInvisibleWindow
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
}
'@

$process = $null
try {
    # Creating the window minimized prevents a visible first-frame flash.  The
    # emulator does not advance reliably while it remains minimized, so make
    # it fully transparent and off-screen before restoring it.
    $process = Start-Process -FilePath $executable `
        -ArgumentList @($EbootPath) `
        -WorkingDirectory $PpssppRoot `
        -WindowStyle Minimized `
        -PassThru

    $window = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 200; ++$attempt) {
        Start-Sleep -Milliseconds 25
        $process.Refresh()
        if ($process.HasExited) {
            throw "PPSSPP exited before its main window became available"
        }
        $window = $process.MainWindowHandle
        if ($window -ne [IntPtr]::Zero) {
            break
        }
    }
    if ($window -eq [IntPtr]::Zero) {
        throw "Timed out waiting for the PPSSPP main window"
    }

    $gwlExStyle = -20
    $wsExTransparent = 0x00000020
    $wsExToolWindow = 0x00000080
    $wsExLayered = 0x00080000
    $wsExNoActivate = 0x08000000
    $lwaAlpha = 0x00000002
    $swRestore = 9
    $swpNoSize = 0x0001
    $swpNoZOrder = 0x0004
    $swpNoActivate = 0x0010

    $style = [PpssppInvisibleWindow]::GetWindowLong($window, $gwlExStyle)
    $style = $style -bor $wsExTransparent -bor $wsExToolWindow `
        -bor $wsExLayered -bor $wsExNoActivate
    [void][PpssppInvisibleWindow]::SetWindowLong($window, $gwlExStyle, $style)
    if (-not [PpssppInvisibleWindow]::SetLayeredWindowAttributes(
            $window, 0, 0, $lwaAlpha)) {
        throw "SetLayeredWindowAttributes failed"
    }

    $positionFlags = $swpNoSize -bor $swpNoZOrder -bor $swpNoActivate
    if (-not [PpssppInvisibleWindow]::SetWindowPos(
            $window, [IntPtr]::Zero, 20000, 20000, 0, 0, $positionFlags)) {
        throw "Initial off-screen SetWindowPos failed"
    }
    [void][PpssppInvisibleWindow]::ShowWindowAsync($window, $swRestore)
    if (-not [PpssppInvisibleWindow]::SetWindowPos(
            $window, [IntPtr]::Zero, 20000, 20000, 0, 0, $positionFlags)) {
        throw "Post-restore off-screen SetWindowPos failed"
    }

    [pscustomobject]@{
        ProcessId = $process.Id
        Window = ("0x{0:x}" -f $window.ToInt64())
        Alpha = 0
        X = 20000
        Y = 20000
    } | ConvertTo-Json -Compress
}
catch {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    throw
}

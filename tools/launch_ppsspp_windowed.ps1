param(
    [Parameter(Mandatory = $true)]
    [string]$PpssppRoot,
    [Parameter(Mandatory = $true)]
    [string]$EbootPath,
    [int]$X = 80,
    [int]$Y = 80
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

public static class PpssppWindowedLaunch
{
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
    $process = Start-Process -FilePath $executable `
        -ArgumentList @($EbootPath) `
        -WorkingDirectory $PpssppRoot `
        -WindowStyle Normal `
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

    $swRestore = 9
    $swpNoSize = 0x0001
    $swpShowWindow = 0x0040
    $notTopmost = [IntPtr](-2)
    [void][PpssppWindowedLaunch]::ShowWindowAsync($window, $swRestore)
    if (-not [PpssppWindowedLaunch]::SetWindowPos(
            $window, $notTopmost, $X, $Y, 0, 0,
            ($swpNoSize -bor $swpShowWindow))) {
        throw "SetWindowPos failed"
    }

    $count = @(Get-Process -Name "PPSSPPWindows64" -ErrorAction SilentlyContinue).Count
    if ($count -ne 1) {
        throw "Single-process invariant failed after launch: count=$count"
    }

    [pscustomobject]@{
        ProcessId = $process.Id
        Window = ("0x{0:x}" -f $window.ToInt64())
        Visible = $true
        X = $X
        Y = $Y
        Topmost = $false
        ProcessCount = $count
    } | ConvertTo-Json -Compress
}
catch {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    throw
}

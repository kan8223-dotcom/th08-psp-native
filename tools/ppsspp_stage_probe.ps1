param(
    [Parameter(Mandatory = $true)][string]$Eboot,
    [Parameter(Mandatory = $true)][string]$Stage,   # 1..3, 4a, 4b, 5, 6a, 6b, or "" for none
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$PlaySeconds = 30,
    [string]$PostKeys = "",
    [int]$PostSeconds = 25
)
# Headless-ish stage start probe: hidden PPSSPP, PostMessage keys, logs + screenshot to artifacts.
$ErrorActionPreference = "Continue"
$root = 'C:/Users/kan82/th08-psp-port/tools/ppsspp-v1.20.4'
$ms = "$root/memstick/PSP/GAME/TH08PSP"
$tools = 'C:/Users/kan82/th08-psp-port/th08-psp-native/tools'
$art = 'C:/Users/kan82/th08-psp-port/artifacts'
Copy-Item -LiteralPath $Eboot -Destination "$ms/EBOOT.PBP" -Force
if ($Stage -ne "") { Set-Content -LiteralPath "$ms/TH08PSP_DEBUG_STAGE.txt" -Value $Stage -NoNewline -Encoding ascii } else { Remove-Item -LiteralPath "$ms/TH08PSP_DEBUG_STAGE.txt" -Force -ErrorAction SilentlyContinue }
Remove-Item -LiteralPath "$ms/TH08PSP_BOOT.LOG","$ms/log.txt" -Force -ErrorAction SilentlyContinue
$launch = & "$tools/launch_ppsspp_invisible.ps1" -PpssppRoot $root -EbootPath "$ms/EBOOT.PBP" | ConvertFrom-Json
$pid2 = $launch.ProcessId
Start-Sleep -Seconds 4
try { & "$tools/set_ppsspp_audio_session.ps1" -Apply -Mute -TargetPid $pid2 | Out-Null } catch { }
Start-Sleep -Seconds 8
# No host keyboard input: the game starts itself via TH08PSP_DEBUG_STAGE.txt "auto".
Start-Sleep -Seconds $PlaySeconds
try { & "$tools/capture_ppsspp_invisible.ps1" -TargetProcessId $pid2 -OutputPath "$art/shots/$Tag.png" | Out-Null } catch { }
try { & "$tools/send_ppsspp_menu_command.ps1" -TargetProcessId $pid2 -CommandId 40010 | Out-Null } catch { }
Start-Sleep -Seconds 3
try { & "$tools/send_ppsspp_menu_command.ps1" -TargetProcessId $pid2 -CommandId 40000 | Out-Null } catch { }
Start-Sleep -Seconds 5
$left = @(Get-Process -Name PPSSPPWindows64 -ErrorAction SilentlyContinue); if ($left.Count -ne 0) { $left | Stop-Process -Force; Start-Sleep -Seconds 2 }
if (Test-Path -LiteralPath "$ms/TH08PSP_BOOT.LOG") { Copy-Item -LiteralPath "$ms/TH08PSP_BOOT.LOG" -Destination "$art/TH08PSP_BOOT.$Tag.LOG" -Force; Remove-Item -LiteralPath "$ms/TH08PSP_BOOT.LOG" -Force }
Copy-Item -LiteralPath "$ms/log.txt" -Destination "$art/log.$Tag.txt" -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$ms/TH08PSP_DEBUG_STAGE.txt" -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath "$ms/EBOOT.PRE_R136_e35dd6f6_20260904.PBP" -Destination "$ms/EBOOT.PBP" -Force
"done $Tag"

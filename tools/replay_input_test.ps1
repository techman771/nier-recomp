# replay_input_test.ps1 <seconds> <label> [<exe>] [-Recording <file>]: a muted test run on the second
# monitor that replays recorded controller input (run_boot.ps1 -Record) with a copy of all the saves
# and the player's settings, so the menus and the frame rate are what they were when it was recorded.
# Other NIER_ switches come from the caller. Log: recomp\replay_<label>.log.
param([int]$Seconds = 90, [string]$Label = 'replay', [string]$Exe = 'nier.exe', [string]$Recording = 'kaine_desert.input', [int]$FrameRate = 5, [string]$Record = '', [string]$Saves = '')
$root = Split-Path $PSScriptRoot -Parent
$settings = "$root\recomp\replay_settings.ini"
# The input is replayed by frame number, so the frame rate must be the one it was recorded at
# (-FrameRate, the setting's index: 5 is 240 fps), with VSync off so the display doesn't cap it.
(Get-Content "$root\recomp\build\settings.ini") -replace '^frame_rate = .*', "frame_rate = $FrameRate" -replace '^vsync = .*', 'vsync = 0' | Set-Content $settings -Encoding ascii
$testSave = "$root\recomp\test_save"
if (Test-Path $testSave) { Remove-Item -Recurse -Force $testSave }
# -Saves <folder in recomp\recordings>: the snapshot of the saves the recording was made with, in place of today's
Copy-Item -Recurse $(if ($Saves) { "$root\recomp\recordings\$Saves" } else { "$root\save" }) $testSave
Remove-Item Env:NIER_FULLSCREEN, Env:NIER_60FPS, Env:NIER_30FPS, Env:NIER_RECORD_INPUT, Env:NIER_REPLAY_FRAME, Env:NIER_AUTO_PRESS -ErrorAction SilentlyContinue
$env:NIER_SETTINGS_FILE = $settings; $env:NIER_MONITOR = 'primary'; $env:NIER_MUTE = '1'; $env:NIER_SAVE_DIR = $testSave   # 'primary' is the second monitor here
$env:NIER_IGNORE_PADS = '1'; $env:NIER_SHOW_FPS = '1'; $env:NIER_REPLAY_INPUT = "$root\recomp\recordings\$Recording"
if ($Record) { $env:NIER_RECORD_INPUT = "$root/recomp/recordings/$Record" }   # -Record <file>: also record what the game is given (to test replays)
$log = "$root\recomp\replay_$Label.log"
$p = Start-Process -FilePath "$root\recomp\build\$Exe" -WorkingDirectory "$root\recomp\build" -RedirectStandardOutput $log -RedirectStandardError "$log.err" -NoNewWindow -PassThru
$null = $p.Handle
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill() }
Remove-Item Env:NIER_SETTINGS_FILE, Env:NIER_MONITOR, Env:NIER_MUTE, Env:NIER_SAVE_DIR, Env:NIER_IGNORE_PADS, Env:NIER_SHOW_FPS, Env:NIER_REPLAY_INPUT, Env:NIER_RECORD_INPUT -ErrorAction SilentlyContinue
(Select-String -Path $log -Pattern '^\[perf\] frame (\d+): ([\d.]+) fps' | Select-Object -Last 3 | ForEach-Object { $_.Matches[0].Value })

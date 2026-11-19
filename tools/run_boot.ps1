# Runs nier.exe with a time limit, writing output to recomp/boot.log. Usage: run_boot.ps1 [seconds] [-Trace]
param([int]$Seconds = 15, [switch]$Trace, [int]$DumpAfter = 0, [string]$SaveFrames = '', [string]$AutoPress = '', [int]$LogDrawsAt = 0, [switch]$Sound, [switch]$Fps30, [switch]$Playtest, [switch]$AskName, [int]$SaveSlot = 0, [string]$Record = '', [string]$Replay = '', [switch]$IgnorePads)
# -Record <file> writes the pad input given to the game; -Replay <file> plays such a file back (test runs).
# -Playtest: a run for the user to play: sound, primary monitor, and the user's own settings.ini
# (display mode, frame rate etc. from Options > Game Settings > PC Settings). Test runs instead use
# recomp\test_settings.ini, stay windowed and unfocused (NIER_FULLSCREEN=0), and run at 60 fps
# unless -Fps30.
Remove-Item Env:NIER_60FPS, Env:NIER_30FPS -ErrorAction SilentlyContinue
if ($Playtest) {
    $Sound = $true
    $env:NIER_LOG_STALLS = '1'   # a stall of the game's main loop (100 ms) logs what every guest thread was doing
    Remove-Item Env:NIER_MONITOR, Env:NIER_FULLSCREEN, Env:NIER_SETTINGS_FILE -ErrorAction SilentlyContinue
} else {
    $env:NIER_FULLSCREEN = '0'
    $env:NIER_SETTINGS_FILE = Join-Path (Split-Path $PSScriptRoot -Parent) 'recomp\test_settings.ini'
    if ($Fps30) { $env:NIER_30FPS = '1' } else { $env:NIER_60FPS = '1' }
}
# The hero's name: test runs answer the on-screen keyboard in advance; a playtest, or -AskName, shows it.
if ($Playtest -or $AskName) { Remove-Item Env:NIER_PLAYER_NAME -ErrorAction SilentlyContinue } elseif (-not $env:NIER_PLAYER_NAME) { $env:NIER_PLAYER_NAME = 'Nier' }
if (-not $Playtest -and -not $env:NIER_MONITOR) { $env:NIER_MONITOR = 'secondary' }      # keeps test windows off the main screen
if ($Sound) { Remove-Item Env:NIER_MUTE -ErrorAction SilentlyContinue } else { $env:NIER_MUTE = '1' }   # test runs stay silent
if ($LogDrawsAt -gt 0) { $env:NIER_LOG_DRAWS_AT = "$LogDrawsAt" } else { Remove-Item Env:NIER_LOG_DRAWS_AT -ErrorAction SilentlyContinue }
# Test runs ignore real controllers (the player may be holding one); a recording run and a playtest read them
# (-IgnorePads: not even then, for a recording made from scripted input).
if (($Playtest -or $Record) -and -not $IgnorePads) { Remove-Item Env:NIER_IGNORE_PADS -ErrorAction SilentlyContinue } else { $env:NIER_IGNORE_PADS = '1' }
if ($Record) { $env:NIER_RECORD_INPUT = $Record } else { Remove-Item Env:NIER_RECORD_INPUT -ErrorAction SilentlyContinue }
if ($Replay) { $env:NIER_REPLAY_INPUT = $Replay } else { Remove-Item Env:NIER_REPLAY_INPUT -ErrorAction SilentlyContinue }
if ($AutoPress) { $env:NIER_AUTO_PRESS = $AutoPress } else { Remove-Item Env:NIER_AUTO_PRESS -ErrorAction SilentlyContinue }
if ($SaveFrames) { $env:NIER_SAVE_FRAMES = $SaveFrames } else { Remove-Item Env:NIER_SAVE_FRAMES -ErrorAction SilentlyContinue }
$root = Split-Path $PSScriptRoot -Parent
# Test runs use a fresh copy of the user's saves (recomp\test_save, refreshed every run), so they can
# load the playthrough's progress (Game 1) without ever writing to save\. -Playtest uses save\.
if ($Playtest) { Remove-Item Env:NIER_SAVE_DIR -ErrorAction SilentlyContinue } else {
    $testSave = Join-Path $root 'recomp\test_save'
    if (Test-Path $testSave) { Remove-Item -Recurse -Force $testSave }
    Copy-Item -Recurse (Join-Path $root 'save') $testSave
    # -SaveSlot N (1-3) keeps only that game save in the copy, so the load list's cursor can only be
    # on it (scripted routes: Continue, A to select, A to confirm).
    if ($SaveSlot -gt 0) {
        Get-ChildItem $testSave -Filter 'GAME_DATA*' | Where-Object { $_.Name -notmatch "^GAME_DATA$($SaveSlot - 1)(\.content)?$" } | Remove-Item -Recurse -Force
    }
    $env:NIER_SAVE_DIR = $testSave
}
$exe = Join-Path $root 'recomp\build\nier.exe'
# A playtest logs to its own file: test runs made while the user plays would otherwise share boot.log.
$log = Join-Path $root $(if ($Playtest) { 'recomp\playtest.log' } else { 'recomp\boot.log' })
if ($Trace) { $env:NIER_TRACE_IMPORTS = '1' } else { Remove-Item Env:NIER_TRACE_IMPORTS -ErrorAction SilentlyContinue }
if ($DumpAfter -gt 0) { $env:NIER_DUMP_AFTER = "$DumpAfter" } else { Remove-Item Env:NIER_DUMP_AFTER -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath $exe -ArgumentList "`"$root\Nier`"" -WorkingDirectory (Split-Path $exe) -RedirectStandardOutput $log -RedirectStandardError "$log.err" -NoNewWindow -PassThru
$null = $p.Handle   # keeps the exit code readable after the process ends
if (-not $p.WaitForExit($Seconds * 1000)) {
    $p.Kill()
    "time limit reached after $Seconds seconds (process killed)"
} else {
    "exit code: $($p.ExitCode)"
}

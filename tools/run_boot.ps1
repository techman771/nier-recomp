# Runs nier.exe with a time limit, writing output to recomp/boot.log. Usage: run_boot.ps1 [seconds] [-Trace]
param([int]$Seconds = 15, [switch]$Trace, [int]$DumpAfter = 0, [string]$SaveFrames = '', [string]$AutoPress = '', [int]$LogDrawsAt = 0, [switch]$Sound)
if (-not $env:NIER_PLAYER_NAME) { $env:NIER_PLAYER_NAME = 'Nier' }   # skips the name window
if ($Sound) { Remove-Item Env:NIER_MUTE -ErrorAction SilentlyContinue } else { $env:NIER_MUTE = '1' }   # test runs stay silent
if ($LogDrawsAt -gt 0) { $env:NIER_LOG_DRAWS_AT = "$LogDrawsAt" } else { Remove-Item Env:NIER_LOG_DRAWS_AT -ErrorAction SilentlyContinue }
if ($AutoPress) { $env:NIER_AUTO_PRESS = $AutoPress } else { Remove-Item Env:NIER_AUTO_PRESS -ErrorAction SilentlyContinue }
if ($SaveFrames) { $env:NIER_SAVE_FRAMES = $SaveFrames } else { Remove-Item Env:NIER_SAVE_FRAMES -ErrorAction SilentlyContinue }
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'recomp\build\nier.exe'
$log = Join-Path $root 'recomp\boot.log'
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

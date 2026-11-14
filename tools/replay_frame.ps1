# Renders a frame capture (recomp\captures\frame_f<N>.nfrm, written when F12 is pressed in the game)
# without running the game, and saves the picture as a PNG. See recomp\runtime\framecapture.cpp.
#   replay_frame.ps1 <capture> [-Out <png>] [-Frames 40] [-Zoom 'z,x,y'] [-Skip '3,10-14'] [-Stop n]
#                    [-PsDebug '<hash>=<expr>'] [-Patch '<hash>|<find>|<replace>'] [-KeepHlsl] [-Show]
#                    [-Cull 'mode,ccw'] (force face culling: 0 none, 1 front, 2 back; ccw 1 or 0)
# -Show leaves the window open until it is closed instead of saving a picture and exiting.
param([Parameter(Mandatory)][string]$Capture, [string]$Out = '', [int]$Frames = 40, [string]$Zoom = '', [string]$Skip = '',
      [int]$Stop = -1, [string]$PsDebug = '', [string]$Patch = '', [switch]$KeepHlsl, [switch]$Show, [string]$Monitor = 'secondary', [string]$Cull = '')
$root = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path $Capture)) { $Capture = Join-Path $root "recomp\captures\$Capture" }
$Capture = (Resolve-Path $Capture).Path
$names = 'NIER_REPLAY_FRAME', 'NIER_REPLAY_FRAMES', 'NIER_REPLAY_ZOOM', 'NIER_REPLAY_SKIP', 'NIER_REPLAY_STOP', 'NIER_REPLAY_KEEP_HLSL', 'NIER_REPLAY_CULL',
         'NIER_PS_DEBUG', 'NIER_SHADER_PATCH', 'NIER_SAVE_FRAMES', 'NIER_AUTO_PRESS', 'NIER_REPLAY_INPUT', 'NIER_RECORD_INPUT'
foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
$env:NIER_REPLAY_FRAME = $Capture
$env:NIER_MUTE = '1'
$env:NIER_FULLSCREEN = '0'
$env:NIER_MONITOR = $Monitor
$env:NIER_SETTINGS_FILE = Join-Path $root 'recomp\test_settings.ini'
if (-not $Show) { $env:NIER_REPLAY_FRAMES = "$Frames"; $env:NIER_SAVE_FRAMES = "$Frames" }
if ($Zoom) { $env:NIER_REPLAY_ZOOM = $Zoom }
if ($Skip) { $env:NIER_REPLAY_SKIP = $Skip }
if ($Stop -ge 0) { $env:NIER_REPLAY_STOP = "$Stop" }
if ($KeepHlsl) { $env:NIER_REPLAY_KEEP_HLSL = '1' }
if ($Cull) { $env:NIER_REPLAY_CULL = $Cull }
if ($PsDebug) { $env:NIER_PS_DEBUG = $PsDebug }
if ($Patch) { $env:NIER_SHADER_PATCH = $Patch }
$exe = Join-Path $root 'recomp\build\nier.exe'
$log = Join-Path $root 'recomp\replay.log'
$frameFile = Join-Path $root "recomp\frames\f$Frames.ppm"
if (-not $Show) { Remove-Item $frameFile -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -RedirectStandardOutput $log -RedirectStandardError "$log.err" -NoNewWindow -PassThru
$null = $p.Handle
if (-not $p.WaitForExit($(if ($Show) { 3600000 } else { 120000 }))) { $p.Kill(); 'time limit reached (process killed)' }
foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
Remove-Item (Join-Path $root 'recomp\test_settings.ini') -ErrorAction SilentlyContinue
if ($Show) { return }
if (-not (Test-Path $frameFile)) { "no picture was saved; see $log"; return }
if (-not $Out) { $Out = [System.IO.Path]::ChangeExtension($Capture, '.png') }
python -I (Join-Path $PSScriptRoot 'ppm2png.py') $frameFile $Out
Remove-Item $frameFile

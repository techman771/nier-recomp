# Renders a frame capture (recomp\captures\frame_f<N>.nfrm, written when F12 is pressed in the game)
# without running the game, and saves the picture as a PNG. See recomp\runtime\framecapture.cpp.
#   replay_frame.ps1 <capture> [-Out <png>] [-Frames 40] [-Zoom 'z,x,y'] [-Skip '3,10-14'] [-Stop n]
#                    [-PsDebug '<hash>=<expr>'] [-Patch '<hash>|<find>|<replace>'] [-KeepHlsl] [-Show]
#                    [-Cull 'mode,ccw'] (force face culling: 0 none, 1 front, 2 back; ccw 1 or 0)
#                    [-Surface <hex address>] (show that resolved surface instead of the picture)
# -Show leaves the window open until it is closed instead of saving a picture and exiting.
# A recording (seq_f<N>.nseq, F9 in the game: every frame for 10 seconds) is played once and every
# -Every'th frame saved as .ppm into <capture>_frames\ or -Out (-Every 1 for all); -Show loops it.
param([Parameter(Mandatory)][string]$Capture, [string]$Out = '', [int]$Frames = 40, [string]$Zoom = '', [string]$Skip = '',
      [int]$Stop = -1, [string]$PsDebug = '', [string]$Patch = '', [switch]$KeepHlsl, [switch]$Show, [string]$Monitor = 'secondary',
      [string]$Cull = '', [int]$Every = 6, [string]$Surface = '')
$root = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path $Capture)) { $Capture = Join-Path $root "recomp\captures\$Capture" }
$Capture = (Resolve-Path $Capture).Path
$names = 'NIER_REPLAY_FRAME', 'NIER_REPLAY_FRAMES', 'NIER_REPLAY_ONCE', 'NIER_REPLAY_ZOOM', 'NIER_REPLAY_SKIP', 'NIER_REPLAY_STOP', 'NIER_REPLAY_KEEP_HLSL',
         'NIER_REPLAY_CULL', 'NIER_SHOW_SURFACE', 'NIER_PS_DEBUG', 'NIER_SHADER_PATCH', 'NIER_SAVE_FRAMES', 'NIER_AUTO_PRESS', 'NIER_REPLAY_INPUT', 'NIER_RECORD_INPUT'
foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
$env:NIER_REPLAY_FRAME = $Capture
$env:NIER_MUTE = '1'
$env:NIER_FULLSCREEN = '0'
$env:NIER_MONITOR = $Monitor
$env:NIER_SETTINGS_FILE = Join-Path $root 'recomp\test_settings.ini'
$sequence = $Capture -like '*.nseq'
$saved = @($Frames)
if ($sequence) {
    # The recording's summary (.txt beside it) ends with its frame count.
    $summary = [System.IO.Path]::ChangeExtension($Capture, '.txt')
    $count = 0
    if (Test-Path $summary) {
        $line = Select-String -Path $summary -Pattern '^frames: (\d+)' | Select-Object -Last 1
        if ($line) { $count = [int]$line.Matches[0].Groups[1].Value }
    }
    if ($count -le 0) { $count = 600 }
    $saved = @(1..$count | Where-Object { ($_ - 1) % $Every -eq 0 })
    if (-not $Show) { $env:NIER_REPLAY_ONCE = '1'; $env:NIER_SAVE_FRAMES = ($saved -join ',') }
} elseif (-not $Show) { $env:NIER_REPLAY_FRAMES = "$Frames"; $env:NIER_SAVE_FRAMES = "$Frames" }
if ($Zoom) { $env:NIER_REPLAY_ZOOM = $Zoom }
if ($Skip) { $env:NIER_REPLAY_SKIP = $Skip }
if ($Stop -ge 0) { $env:NIER_REPLAY_STOP = "$Stop" }
if ($KeepHlsl) { $env:NIER_REPLAY_KEEP_HLSL = '1' }
if ($Cull) { $env:NIER_REPLAY_CULL = $Cull }
if ($Surface) { $env:NIER_SHOW_SURFACE = $Surface }
if ($PsDebug) { $env:NIER_PS_DEBUG = $PsDebug }
if ($Patch) { $env:NIER_SHADER_PATCH = $Patch }
$exe = Join-Path $root 'recomp\build\nier.exe'
$log = Join-Path $root 'recomp\replay.log'
$frameDir = Join-Path $root 'recomp\frames'
$files = $saved | ForEach-Object { Join-Path $frameDir "f$_.ppm" }
if (-not $Show) { $files | ForEach-Object { Remove-Item $_ -ErrorAction SilentlyContinue } }
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -RedirectStandardOutput $log -RedirectStandardError "$log.err" -NoNewWindow -PassThru
$null = $p.Handle
if (-not $p.WaitForExit($(if ($Show) { 3600000 } else { 180000 }))) { $p.Kill(); 'time limit reached (process killed)' }
foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
Remove-Item (Join-Path $root 'recomp\test_settings.ini') -ErrorAction SilentlyContinue
if ($Show) { return }
if ($sequence) {
    if (-not $Out) { $Out = [System.IO.Path]::ChangeExtension($Capture, $null).TrimEnd('.') + '_frames' }
    New-Item -ItemType Directory -Force $Out | Out-Null
    Remove-Item (Join-Path $Out 'f*.ppm') -ErrorAction SilentlyContinue
    $moved = 0
    foreach ($file in $files) { if (Test-Path $file) { Move-Item $file $Out -Force; $moved++ } }
    "$moved frames in $Out"
    return
}
$frameFile = $files[0]
if (-not (Test-Path $frameFile)) { "no picture was saved; see $log"; return }
if (-not $Out) { $Out = [System.IO.Path]::ChangeExtension($Capture, '.png') }
python -I (Join-Path $PSScriptRoot 'ppm2png.py') $frameFile $Out
Remove-Item $frameFile

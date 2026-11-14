# Fetches the SDL3 development package (headers, import library, SDL3.dll) into tools\SDL3.
# The runtime uses SDL3 for controllers (recomp/runtime/input.cpp). tools\SDL3 is not committed.
$version = '3.4.20'
$sha256 = '1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969'
$target = Join-Path $PSScriptRoot 'SDL3'
if (Test-Path (Join-Path $target 'include\SDL3\SDL.h')) { "SDL3 already present in $target"; exit 0 }
$zip = Join-Path $env:TEMP "SDL3-devel-$version-VC.zip"
Invoke-WebRequest -Uri "https://github.com/libsdl-org/SDL/releases/download/release-$version/SDL3-devel-$version-VC.zip" -OutFile $zip -UseBasicParsing
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($hash -ne $sha256) { throw "SDL3 download hash mismatch: $hash" }
$unpacked = Join-Path $env:TEMP "sdl3-unpack-$version"
Expand-Archive $zip -DestinationPath $unpacked -Force
Move-Item (Join-Path $unpacked "SDL3-$version") $target
"SDL3 $version unpacked into $target"

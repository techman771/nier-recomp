# Fetches NVIDIA's Streamline SDK (276 MB) into tools\Streamline\sdk: the headers and import library
# the runtime is built with for DLSS (recomp/runtime/dlss.cpp), and the DLLs copied beside nier.exe
# by compile_ppc.ps1 (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll).
# Nothing runs this unasked: without the SDK the game is built without DLSS and FSR does the work.
# tools\Streamline is not committed. Streamline's own code is MIT licensed; nvngx_dlss.dll is
# NVIDIA's DLSS model under NVIDIA's own licence (bin\x64\nvngx_dlss.license.txt in the SDK):
# read it before passing that file on.
$version = '2.14.16'
$sha256 = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
$target = Join-Path $PSScriptRoot 'Streamline'
$sdk = Join-Path $target 'sdk'
if (Test-Path (Join-Path $sdk 'include\sl.h')) { "Streamline already present in $sdk"; exit 0 }
New-Item -ItemType Directory -Force $target | Out-Null
$zip = Join-Path $target "streamline-sdk-v$version.zip"
if (-not (Test-Path $zip)) {
    Invoke-WebRequest -Uri "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/streamline-sdk-v$version.zip" -OutFile $zip -UseBasicParsing
}
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($hash -ne $sha256) { throw "Streamline download hash mismatch: $hash" }
Expand-Archive $zip -DestinationPath $sdk -Force
"Streamline $version unpacked into $sdk"

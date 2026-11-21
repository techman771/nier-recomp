# get_dlss.ps1: fetches the files DLSS needs and puts them beside nier.exe. For players with an
# NVIDIA RTX card; without these files the game simply does not offer DLSS and FSR does the work.
#
# The files are NVIDIA's, not this project's, and are not shipped with it: sl.interposer.dll,
# sl.common.dll and sl.dlss.dll (Streamline, MIT licensed) and nvngx_dlss.dll (the DLSS model, under
# NVIDIA's own licence, saved beside it as nvngx_dlss.license.txt: by using it you accept that).
# They come from NVIDIA's Streamline SDK release on GitHub, a 276 MB download from which only these
# files (60 MB) are kept. The download and each file are checked against known checksums.
#
#   get_dlss.cmd                     double-click: runs this script
#   get_dlss.ps1 [-Zip <file>]       -Zip: a copy of streamline-sdk-v2.14.16.zip you already have
#
# Run from the game's folder (beside nier.exe); from the source tree's tools folder it fills
# recomp\build.
param([string]$Zip = '')
$ErrorActionPreference = 'Stop'
$version = '2.14.16'
$zipSha256 = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
$files = [ordered]@{
    'sl.interposer.dll' = 'b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf'
    'sl.common.dll'     = '38eea3b4209afc455636ea372b5933d65fcc13becec6c84d101dcfc4d8648f75'
    'sl.dlss.dll'       = '18871eea87c90345da97d8109faa81ea7d5840e8d99b51b55b0e46cfa177f84e'
    'nvngx_dlss.dll'    = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'
}
function Get-Sha256([string]$path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() }

$target = $PSScriptRoot
if (-not (Test-Path (Join-Path $target 'nier.exe'))) {
    $build = Join-Path (Split-Path $PSScriptRoot -Parent) 'recomp\build'
    if (Test-Path $build) { $target = $build } else { throw "Put this script beside nier.exe and run it there." }
}

$missing = @($files.Keys | Where-Object { $p = Join-Path $target $_; -not (Test-Path $p) -or (Get-Sha256 $p) -ne $files[$_] })
if ($missing.Count -eq 0) { "The DLSS files are already in $target"; exit 0 }

$fetched = $false
if (-not $Zip) {
    $Zip = Join-Path $target "streamline-sdk-v$version.zip.part"
    "Downloading NVIDIA's Streamline SDK $version (276 MB)..."
    $ProgressPreference = 'SilentlyContinue'   # the progress bar makes Windows PowerShell's download many times slower
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/streamline-sdk-v$version.zip" -OutFile $Zip -UseBasicParsing
    $fetched = $true
}
try {
    $hash = Get-Sha256 $Zip
    if ($hash -ne $zipSha256) { throw "This is not the Streamline SDK $version that was expected (checksum $hash)." }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($name in @($files.Keys) + 'nvngx_dlss.license.txt') {
            $entry = $archive.Entries | Where-Object { $_.FullName -eq "bin/x64/$name" }
            if (-not $entry) { throw "bin/x64/$name is not in the SDK." }
            $to = Join-Path $target $name
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $to, $true)
            if ($files.Contains($name) -and (Get-Sha256 $to) -ne $files[$name]) { Remove-Item $to -Force; throw "$name does not have the expected checksum." }
        }
    } finally { $archive.Dispose() }
} finally {
    if ($fetched -and (Test-Path $Zip)) { Remove-Item $Zip -Force }
}
"The DLSS files are in $target. On an NVIDIA RTX card, DLSS is now under Options > Game Settings > PC Settings > Upscaling."

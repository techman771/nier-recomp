# Regenerates jump tables and leaf function bounds, then assembles recomp/config/nier.toml.
$root = Split-Path $PSScriptRoot -Parent
$cfg = Join-Path $root 'recomp\config'
& (Join-Path $root 'tools\build\xenonscan\jumptables.exe') (Join-Path $root 'Nier\default.xex') "$cfg\nier_switch_tables.toml" "$cfg\nier_functions.snippet.toml"
if ($LASTEXITCODE -ne 0) { throw "jumptables failed: $LASTEXITCODE" }
Get-Content "$cfg\nier.base.toml", "$cfg\nier_functions.snippet.toml" | Set-Content -Encoding ascii "$cfg\nier.toml"

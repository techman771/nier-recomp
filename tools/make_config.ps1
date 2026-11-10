# Regenerates jump tables and leaf function bounds, then assembles recomp/config/nier.toml.
$root = Split-Path $PSScriptRoot -Parent
$cfg = Join-Path $root 'recomp\config'
& (Join-Path $root 'tools\build\xenonscan\jumptables.exe') (Join-Path $root 'Nier\default.xex') "$cfg\nier_switch_tables.toml" "$cfg\nier_functions.snippet.toml"
if ($LASTEXITCODE -ne 0) { throw "jumptables failed: $LASTEXITCODE" }
# Hand-checked bounds (nier_functions.manual.toml) go at the start of the generated functions list.
$manual = Get-Content "$cfg\nier_functions.manual.toml" | Where-Object { $_ -match '^\s*\{' }
$functions = foreach ($line in Get-Content "$cfg\nier_functions.snippet.toml") { $line; if ($line -match '^functions\s*=\s*\[') { $manual } }
(Get-Content "$cfg\nier.base.toml") + $functions | Set-Content -Encoding ascii "$cfg\nier.toml"

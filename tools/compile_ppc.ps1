# Builds recomp/build/nier.exe from the recompiled code and the runtime. Load devshell.ps1 first.
# The runtime is optimized too (it emulates the GPU's command processor, which bounds gameplay speed);
# -RuntimeOpt /Od gives a debuggable runtime. Symbols (/Z7) are kept either way.
param([string]$Opt = '/O2', [string]$RuntimeOpt = '/O2', [string]$CpuArch = '/arch:AVX2')
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'recomp\build'
New-Item -ItemType Directory -Force $build | Out-Null

python -I (Join-Path $PSScriptRoot 'gen_stubs.py') (Join-Path $root 'recomp\out') (Join-Path $root 'recomp\runtime')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sources = Get-ChildItem (Join-Path $root 'recomp\out') -Filter *.cpp | Select-Object -ExpandProperty Name
$runtime = Get-ChildItem (Join-Path $root 'recomp\runtime') -Filter *.cpp | Select-Object -ExpandProperty Name
$x = '../../tools/XenonRecomp'
# /arch:AVX2 (Haswell / Zen and later: AVX2, BMI, FMA; main.cpp checks for it at start). Contraction is
# off so that no multiply and add are fused: the recompiled code's floating point stays exactly as it
# was built and tested with /arch:AVX. -CpuArch /arch:AVX builds for older processors.
$common = "/nologo /c /std:c++20 /MT $CpuArch -msse4.1 -ffp-contract=off -fno-strict-aliasing -Wno-everything -I$x/thirdparty/simde -I../out"
$lib = '../../tools/build'
$sdl = '../../tools/SDL3'   # tools/get_sdl3.ps1 fetches it; SDL3.dll is copied beside nier.exe below
if (-not (Test-Path (Join-Path $PSScriptRoot 'SDL3\include\SDL3\SDL.h'))) { & (Join-Path $PSScriptRoot 'get_sdl3.ps1') }
if (-not (Test-Path (Join-Path $PSScriptRoot 'FidelityFX/sdk/upscalers/include/ffx_upscale.h'))) { & (Join-Path $PSScriptRoot 'get_fsr_sdk.ps1') | Out-Null }
$libs = "$sdl/lib/x64/SDL3.lib $lib/XenonUtils/XenonUtils.lib $lib/thirdparty/disasm/disasm.lib $lib/thirdparty/fmt/fmt.lib $lib/thirdparty/xxHash/cmake_unofficial/xxhash.lib"
$ninja = @(
    "rule cxx",
    "  command = clang-cl $common $Opt /EHs-c- /GS- `$in /Fo:`$out",
    "  description = `$in",
    "",
    "rule rt",
    "  command = clang-cl $common $RuntimeOpt /Z7 /EHsc -I../runtime -I$sdl/include -I../../tools/FidelityFX/sdk -I$x/XenonUtils -I$x/thirdparty -I$x/thirdparty/disasm `$in /Fo:`$out",
    "  description = `$in",
    "",
    "rule link",
    "  command = lld-link /nologo /OUT:`$out /SUBSYSTEM:CONSOLE /STACK:0x10000000,0x100000 @`$out.rsp $libs gdi32.lib gdiplus.lib user32.lib winmm.lib ole32.lib xinput.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib dbghelp.lib advapi32.lib delayimp.lib /DELAYLOAD:d3dcompiler_47.dll /DEBUG:FULL",
    "  rspfile = `$out.rsp",
    "  rspfile_content = `$in",
    "  description = linking `$out",
    ""
)
$objects = @()
foreach ($s in $sources) { $ninja += "build $s.obj: cxx ../out/$s"; $objects += "$s.obj" }
# Every runtime header is an implicit dependency (no compiler dependency tracking here), so a header
# change rebuilds all runtime files.
$headers = (Get-ChildItem (Join-Path $root 'recomp\runtime') -Filter *.h | ForEach-Object { "../runtime/$($_.Name)" }) -join ' '
foreach ($s in $runtime) { $ninja += "build rt_$s.obj: rt ../runtime/$s | $headers"; $objects += "rt_$s.obj" }
$ninja += "build nier.exe: link $($objects -join ' ')"
Set-Content -Encoding ascii (Join-Path $build 'build.ninja') $ninja
ninja -C $build -k 0
$result = $LASTEXITCODE
Copy-Item (Join-Path $PSScriptRoot 'SDL3/lib/x64/SDL3.dll') $build -Force
# AMD's FSR 1 shader headers (the Upscaling setting compiles its passes from them at runtime).
if (-not (Test-Path (Join-Path $PSScriptRoot 'FidelityFX/fsr1/ffx_fsr1.h'))) { & (Join-Path $PSScriptRoot 'get_fsr1.ps1') | Out-Null }
New-Item -ItemType Directory -Force (Join-Path $build 'fsr1') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'FidelityFX/fsr1/*') (Join-Path $build 'fsr1') -Force
# AMD's FSR SDK DLLs (the Upscaling setting's FSR 3.1); copied when missing or changed, they are large.
foreach ($dll in 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll') {
    $from = Join-Path $PSScriptRoot "FidelityFX/sdk/signedbin/$dll"; $to = Join-Path $build $dll
    if ((Test-Path $from) -and (-not (Test-Path $to) -or (Get-Item $from).Length -ne (Get-Item $to).Length)) { Copy-Item $from $to -Force }
}
# Button glyph atlases (PlayStation, Xbox Series, keyboard), drawn by make_glyphs.ps1.
$glyphScript = Join-Path $PSScriptRoot 'make_glyphs.ps1'
$glyphOut = Join-Path $build 'glyphs\xbox.rgba'
if (-not (Test-Path $glyphOut) -or (Get-Item $glyphScript).LastWriteTime -gt (Get-Item $glyphOut).LastWriteTime) { & $glyphScript | Out-Null }
exit $result
